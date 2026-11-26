#pragma once
// Waits for a normally launched client and runs one backend per client
// process. It never starts, copies or terminates the game. Native port of the
// former managed/Start-Standalone.ps1 watcher, plus a live volume pipe.
#include <windows.h>
#include <TlHelp32.h>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <algorithm>
#include "../settings.h"
#include "../i18n.h"
#include "app_config.h"

namespace lazer {

enum class WatchPhase { Stopped, Waiting, Attaching, Ready, Failed, Released };
enum class ContractKind { None, Verified, Adaptive };

// Stable backend markers. Display text after the marker is translated and is
// never used to infer success.
struct BackendLine { bool ready = false; ContractKind contract = ContractKind::None; };
inline BackendLine parseBackendLine(const std::string &line)
{
    BackendLine result;
    if (line.rfind("[ASIO_READY]", 0) == 0) result.ready = true;
    else if (line.rfind("[Verified Contract]", 0) == 0) result.contract = ContractKind::Verified;
    else if (line.rfind("[Compat/Adaptive]", 0) == 0) result.contract = ContractKind::Adaptive;
    return result;
}

struct WatchSnapshot {
    WatchPhase phase = WatchPhase::Stopped;
    ContractKind contract = ContractKind::None;
    DWORD pid = 0;
    std::wstring gamePath, stderrLog;
    int exitCode = 0;
    bool backendRunning = false;
    asio::Settings running;      // config.ini as read when the backend started
    bool haveRunning = false;
    std::string deviceLine;      // backend's [FMOD] device summary
};

inline std::string wideToUtf8(const std::wstring &text) { return asio::utf8Path(text); }

inline std::string timestampText()
{
    SYSTEMTIME t; GetLocalTime(&t);
    char text[16]; snprintf(text, sizeof(text), "%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    return text;
}

// Managed assemblies carry a CLI header (data directory 14). This stands in
// for the former AssemblyName check; the contract check in the game process
// remains the real compatibility gate.
inline bool isManagedImage(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    unsigned char header[4096] = {};
    DWORD read = 0;
    bool ok = ReadFile(file, header, sizeof(header), &read, nullptr) && read >= 512;
    CloseHandle(file);
    if (!ok || header[0] != 'M' || header[1] != 'Z') return false;
    LONG pe = *reinterpret_cast<LONG *>(header + 0x3c);
    if (pe <= 0 || DWORD(pe) + 24 + 2 > read) return false;
    if (memcmp(header + pe, "PE\0\0", 4)) return false;
    const unsigned char *optional = header + pe + 24;
    WORD magic = *reinterpret_cast<const WORD *>(optional);
    DWORD directories = magic == 0x20b ? 112 : magic == 0x10b ? 96 : 0;
    if (!directories || DWORD(pe) + 24 + directories + 15 * 8 > read) return false;
    const DWORD *cli = reinterpret_cast<const DWORD *>(optional + directories + 14 * 8);
    return cli[0] != 0 && cli[1] >= 72;
}

inline bool looksLikeLazerClient(const std::wstring &exePath)
{
    size_t slash = exePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    const std::wstring directory = exePath.substr(0, slash + 1);
    for (const wchar_t *name : {L"osu!.dll", L"osu.Game.dll", L"osu.Framework.dll", L"coreclr.dll", L"hostfxr.dll"}) {
        DWORD attributes = GetFileAttributesW((directory + name).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    }
    return isManagedImage(directory + L"osu.Game.dll") && isManagedImage(directory + L"osu.Framework.dll");
}

// Files the backend and the in-game bridge load from the tool folder. Checked
// before anything is injected, so an incomplete copy never touches the game.
inline std::vector<std::string> missingBundleFiles()
{
    std::vector<std::string> missing;
    for (const wchar_t *name : {L"asio_backend.exe", L"osu!asio_sound.dll", L"fmod.dll", L"config.ini", L"managed\\Asio.ManagedBridge.dll",
             L"managed\\Asio.ManagedBridge.deps.json", L"managed\\0Harmony.dll"}) {
        DWORD attributes = GetFileAttributesW(asio::executableSibling(name).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) missing.push_back(asio::utf8Path(name));
    }
    return missing;
}

class GameWatcher {
public:
    using Notify = std::function<void()>;
    ~GameWatcher() { stop(); }

    // Fails when another watcher (window or command line) is already active.
    bool start(const std::wstring &processName, Notify notify, const std::wstring &mutexName = L"Local\\OsuAsioStandaloneWatcher",
               int maximumWaitSeconds = 0, bool once = false)
    {
        if (thread_.joinable()) { if (!finished_) return true; stop(); }
        // Existence, not ownership: a mutex is recursive for its owning thread,
        // and the name must exclude watchers in this process as well as others.
        mutex_ = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (!mutex_) return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex_); mutex_ = nullptr; return false; }
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        volumeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        processName_ = processName;
        notify_ = std::move(notify);
        maximumWait_ = maximumWaitSeconds;
        once_ = once;
        finished_ = false;
        timedOut_ = false;
        result_ = 0;
        {
            std::lock_guard<std::mutex> lock(lock_);
            state_ = {};
            state_.phase = WatchPhase::Waiting;
        }
        thread_ = std::thread([this] { run(); });
        return true;
    }

    // Closing the control pipe asks the backend for its orderly shutdown,
    // which releases ASIO and hands every channel back to native output.
    void stop()
    {
        if (!thread_.joinable()) return;
        SetEvent(stopEvent_);
        thread_.join();
        CloseHandle(stopEvent_); stopEvent_ = nullptr;
        CloseHandle(volumeEvent_); volumeEvent_ = nullptr;
        CloseHandle(mutex_); mutex_ = nullptr;
        std::lock_guard<std::mutex> lock(lock_);
        state_.phase = WatchPhase::Stopped;
        state_.backendRunning = false;
    }

    bool active() const { return thread_.joinable(); }
    bool finished() const { return finished_; }
    bool timedOut() const { return timedOut_; }
    int result() const { return result_; }

    void setVolume(int percent)
    {
        pendingVolume_ = percent;
        if (volumeEvent_) SetEvent(volumeEvent_);
    }

    WatchSnapshot snapshot() const { std::lock_guard<std::mutex> lock(lock_); return state_; }

    std::vector<std::string> takeLog()
    {
        std::lock_guard<std::mutex> lock(lock_);
        std::vector<std::string> lines;
        lines.swap(log_);
        return lines;
    }

private:
    void publish(const std::function<void(WatchSnapshot &)> &change)
    {
        { std::lock_guard<std::mutex> lock(lock_); change(state_); }
        if (notify_) notify_();
    }
    void log(const std::string &line)
    {
        { std::lock_guard<std::mutex> lock(lock_); log_.push_back(timestampText() + "  " + line); if (log_.size() > 400) log_.erase(log_.begin()); }
        if (notify_) notify_();
    }
    bool stopping() const { return WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0; }

    struct Candidate { DWORD pid; ULONGLONG created; std::wstring path; };

    // The client is recognized by its game files, not its executable name, so a
    // renamed EXE needs no setting. An optional name filter remains for tests.
    bool findCandidate(Candidate &found)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        const std::wstring exe = processName_ + L".exe";
        const DWORD self = GetCurrentProcessId();
        std::vector<Candidate> candidates;
        std::vector<wchar_t> path(32768);
        PROCESSENTRY32W entry = {}; entry.dwSize = sizeof(entry);
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
            if (entry.th32ProcessID <= 4 || entry.th32ProcessID == self) continue;
            if (!processName_.empty() && _wcsicmp(entry.szExeFile, exe.c_str())) continue;
            // Cheap skip for processes already rejected by their files.
            auto rejected = rejected_.find(entry.th32ProcessID);
            if (rejected != rejected_.end() && rejected->second == entry.szExeFile) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue; // Retried next pass, like an unreadable Path.
            FILETIME created, exited, kernel, user;
            DWORD length = DWORD(path.size());
            if (GetProcessTimes(process, &created, &exited, &kernel, &user) && QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
                Candidate candidate{entry.th32ProcessID, (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime, std::wstring(path.data(), length)};
                if (seen_.count({candidate.pid, candidate.created})) {}
                else if (looksLikeLazerClient(candidate.path)) candidates.push_back(candidate);
                else rejected_[entry.th32ProcessID] = entry.szExeFile;
            }
            CloseHandle(process);
        }
        CloseHandle(snapshot);
        if (rejected_.size() > 4096) rejected_.clear();
        std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.created < b.created; });
        for (auto &candidate : candidates) {
            auto identity = std::make_pair(candidate.pid, candidate.created);
            if (!runtimeStarted(candidate)) {
                if (announced_.insert(identity).second)
                    log(asio::i18n::text("gui.runtime_wait", {std::to_string(candidate.pid), wideToUtf8(candidate.path)}));
                continue; // Not seen yet: checked again on the next pass.
            }
            seen_.insert(identity);
            found = candidate;
            return true;
        }
        return false;
    }

    // Inject only after the client's .NET runtime is executing managed code:
    // coreclr.dll and the JIT are loaded and the process is a few seconds old.
    static bool runtimeStarted(const Candidate &candidate)
    {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULONGLONG current = (ULONGLONG(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        const ULONGLONG age = current > candidate.created ? current - candidate.created : 0; // 100 ns units
        if (age < 20000000ULL) return false;
        HANDLE modules = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 3 && modules == INVALID_HANDLE_VALUE; ++attempt) {
            modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, candidate.pid);
            if (modules == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) return false;
        }
        if (modules == INVALID_HANDLE_VALUE) return false;
        bool coreclr = false, jit = false;
        MODULEENTRY32W module = {}; module.dwSize = sizeof(module);
        for (BOOL more = Module32FirstW(modules, &module); more; more = Module32NextW(modules, &module)) {
            coreclr |= _wcsicmp(module.szModule, L"coreclr.dll") == 0;
            jit |= _wcsicmp(module.szModule, L"clrjit.dll") == 0;
        }
        CloseHandle(modules);
        // A fully precompiled client might never load the JIT; give it 8 s.
        return coreclr && (jit || age >= 80000000ULL);
    }

    std::vector<std::string> tail(const std::wstring &file, size_t count)
    {
        std::string text;
        if (!asio::i18n::readUtf8(file, text)) return {};
        std::vector<std::string> lines;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(line);
            start = end + 1;
        }
        if (lines.size() > count) lines.erase(lines.begin(), lines.end() - count);
        return lines;
    }

    void attach(const Candidate &game)
    {
        namespace ui = asio::i18n;
        const std::string gamePath = wideToUtf8(game.path);
        log(ui::text("watcher.found", {std::to_string(game.pid), gamePath}));
        asio::Settings running;
        std::string error;
        bool haveRunning = asio::loadSettings(asio::executableSibling(L"config.ini"), running, error);
        // A hand-edited Strict value is normalized before the backend reads it.
        if (haveRunning && running.contractMode != 1) haveRunning = updateAudioSettings(running, [](asio::Settings &) {});
        publish([&](WatchSnapshot &s) {
            s.phase = WatchPhase::Attaching; s.pid = game.pid; s.gamePath = game.path; s.contract = ContractKind::None;
            s.exitCode = 0; s.backendRunning = true; s.running = running; s.haveRunning = haveRunning; s.deviceLine.clear();
        });

        const std::wstring logs = asio::executableSibling(L"logs");
        CreateDirectoryW(logs.c_str(), nullptr);
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t stamp[64];
        swprintf(stamp, 64, L"%lu-%04u%02u%02u-%02u%02u%02u-%03u", game.pid, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        const std::wstring stdoutPath = logs + L"\\attach-" + stamp + L".stdout.log";
        const std::wstring stderrPath = logs + L"\\attach-" + stamp + L".stderr.log";

        SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        HANDLE out = CreateFileW(stdoutPath.c_str(), GENERIC_WRITE, share, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE err = CreateFileW(stderrPath.c_str(), GENERIC_WRITE, share, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE childInput = nullptr, controlWrite = nullptr;
        bool pipes = CreatePipe(&childInput, &controlWrite, &inherit, 0) && SetHandleInformation(controlWrite, HANDLE_FLAG_INHERIT, 0);
        HANDLE reader = CreateFileW(stdoutPath.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        PROCESS_INFORMATION child = {};
        bool started = false;
        if (out != INVALID_HANDLE_VALUE && err != INVALID_HANDLE_VALUE && pipes && reader != INVALID_HANDLE_VALUE) {
            std::wstring environment = childEnvironment();
            std::wstring backend = asio::executableSibling(L"asio_backend.exe");
            std::wstring command = L"\"" + backend + L"\" --live --attach-pid " + std::to_wstring(game.pid) + L" --control-stdin";
            STARTUPINFOW startup = {}; startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = childInput; startup.hStdOutput = out; startup.hStdError = err;
            started = CreateProcessW(backend.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                environment.data(), asio::executableDirectory().c_str(), &startup, &child) != FALSE;
            if (!started) log(ui::text("gui.backend_start_failed", {std::to_string(GetLastError())}));
        }
        for (HANDLE handle : {out, err, childInput}) if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (!started) {
            if (controlWrite) CloseHandle(controlWrite);
            if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
            publish([&](WatchSnapshot &s) { s.phase = WatchPhase::Failed; s.exitCode = -1; s.backendRunning = false; s.stderrLog = stderrPath; });
            return;
        }
        CloseHandle(child.hThread);
        sendVolume(controlWrite);

        bool ready = false, stopRequested = false;
        std::string pending;
        auto drain = [&] {
            char buffer[4096]; DWORD got = 0;
            while (ReadFile(reader, buffer, sizeof(buffer), &got, nullptr) && got) {
                pending.append(buffer, got);
                if (pending.compare(0, 3, "\xef\xbb\xbf") == 0) pending.erase(0, 3);
                size_t newline;
                while ((newline = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, newline);
                    pending.erase(0, newline + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    BackendLine parsed = parseBackendLine(line);
                    if (parsed.contract != ContractKind::None) publish([&](WatchSnapshot &s) { s.contract = parsed.contract; });
                    if (parsed.ready) { ready = true; publish([&](WatchSnapshot &s) { s.phase = WatchPhase::Ready; }); }
                    if (line.rfind("[FMOD] ", 0) == 0 && line.find("driver=") != std::string::npos)
                        publish([&](WatchSnapshot &s) { s.deviceLine = line.substr(7); });
                    log(line);
                }
            }
        };
        HANDLE waits[3] = {child.hProcess, stopEvent_, volumeEvent_};
        for (;;) {
            DWORD wait = WaitForMultipleObjects(stopRequested ? 1 : 3, waits, FALSE, 150);
            drain();
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_OBJECT_0 + 2) sendVolume(controlWrite);
            if (wait == WAIT_OBJECT_0 + 1 && !stopRequested) {
                stopRequested = true;
                CloseHandle(controlWrite); controlWrite = nullptr;
                // Before readiness the backend is not reading its pipe yet;
                // bound the wait exactly as the former watcher's Kill did.
                if (WaitForSingleObject(child.hProcess, 3000) == WAIT_TIMEOUT) TerminateProcess(child.hProcess, 1);
            }
        }
        if (controlWrite) CloseHandle(controlWrite);
        drain();
        CloseHandle(reader);
        DWORD exitCode = 1;
        GetExitCodeProcess(child.hProcess, &exitCode);
        CloseHandle(child.hProcess);
        if (stopRequested) {
            log(ui::text("gui.backend_stopped"));
            publish([&](WatchSnapshot &s) { s.backendRunning = false; s.exitCode = int(exitCode); });
            return;
        }
        if (exitCode != 0 || !ready) {
            log(ui::text("watcher.attach_failed", {std::to_string(exitCode), wideToUtf8(stderrPath)}));
            bool bridgeLoad = false;
            for (auto &line : tail(stderrPath, 8)) { bridgeLoad |= line.find("0x80131509") != std::string::npos; log(line); }
            // The hook and bridge write their own causes inside the game process.
            const std::wstring managedLog = logs + L"\\managed-" + std::to_wstring(game.pid) + L".log";
            for (auto &line : tail(managedLog, 8)) log("[managed] " + line);
            for (auto &line : tail(asio::executableSibling(L"asio_hook.log"), 6)) log("[hook] " + line);
            if (bridgeLoad) log(ui::text("gui.hint_bridge_load"));
            if (GetFileAttributesW(managedLog.c_str()) == INVALID_FILE_ATTRIBUTES) log(ui::text("gui.hint_debug_log"));
            result_ = 1;
            publish([&](WatchSnapshot &s) { s.phase = WatchPhase::Failed; s.exitCode = int(exitCode); s.backendRunning = false; s.stderrLog = stderrPath; });
        } else {
            log(ui::text("watcher.released"));
            publish([&](WatchSnapshot &s) { s.phase = WatchPhase::Released; s.exitCode = 0; s.backendRunning = false; });
        }
    }

    void sendVolume(HANDLE pipe)
    {
        int volume = pendingVolume_.exchange(-1);
        if (volume < 0 || !pipe) return;
        std::string line = "volume " + std::to_string(volume) + "\n";
        DWORD written = 0;
        WriteFile(pipe, line.data(), DWORD(line.size()), &written, nullptr);
    }

    static std::wstring childEnvironment()
    {
        std::wstring block;
        wchar_t *current = GetEnvironmentStringsW();
        for (const wchar_t *entry = current; entry && *entry; entry += wcslen(entry) + 1) {
            if (!_wcsnicmp(entry, L"ASIO_STANDALONE_ENABLE=", 23) || !_wcsnicmp(entry, L"ASIO_PLAYBACK_TRACE=", 20)) continue;
            block += entry; block.push_back(L'\0');
        }
        if (current) FreeEnvironmentStringsW(current);
        block += L"ASIO_STANDALONE_ENABLE=1"; block.push_back(L'\0');
        const auto trace = asio::executableSibling(L"playback-trace.enabled");
        if (GetFileAttributesW(trace.c_str()) != INVALID_FILE_ATTRIBUTES) { block += L"ASIO_PLAYBACK_TRACE=1"; block.push_back(L'\0'); }
        block.push_back(L'\0');
        return block;
    }

    void run()
    {
        namespace ui = asio::i18n;
        log(processName_.empty() ? ui::text("gui.watch_auto") : ui::text("watcher.waiting", {wideToUtf8(processName_)}));
        log(ui::text("watcher.behavior"));
        if (GetFileAttributesW(asio::executableSibling(L"playback-trace.enabled").c_str()) != INVALID_FILE_ATTRIBUTES) log(ui::text("watcher.trace"));
        const ULONGLONG deadline = maximumWait_ > 0 ? GetTickCount64() + ULONGLONG(maximumWait_) * 1000 : ~0ULL;
        while (!stopping()) {
            if (GetTickCount64() >= deadline) { timedOut_ = true; result_ = 1; break; }
            Candidate game;
            if (!findCandidate(game)) { WaitForSingleObject(stopEvent_, 250); continue; }
            attach(game);
            if (once_ || stopping()) break;
            log(ui::text("watcher.continue"));
            publish([](WatchSnapshot &s) { if (s.phase == WatchPhase::Released) s.phase = WatchPhase::Waiting; });
        }
        finished_ = true;
        if (notify_) notify_();
    }

    mutable std::mutex lock_;
    WatchSnapshot state_;
    std::vector<std::string> log_;
    std::thread thread_;
    HANDLE mutex_ = nullptr, stopEvent_ = nullptr, volumeEvent_ = nullptr;
    std::wstring processName_;
    Notify notify_;
    std::set<std::pair<DWORD, ULONGLONG>> seen_, announced_;
    std::unordered_map<DWORD, std::wstring> rejected_;
    std::atomic<int> pendingVolume_{-1};
    std::atomic<bool> finished_{false}, timedOut_{false};
    std::atomic<int> result_{0};
    int maximumWait_ = 0;
    bool once_ = false;
};

} // namespace lazer
