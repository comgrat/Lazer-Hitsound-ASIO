// Window-side helpers without a game, backend or audio device: readiness
// markers, client recognition, watcher exclusivity/timeout and tool files.
#include "../gui/watcher.h"
#include "../gui/app_config.h"
#include <cstdio>
#include <vector>

static int checks = 0, failures = 0;
static void check(bool pass, const char *name)
{
    ++checks;
    if (!pass) ++failures;
    printf("%s: %s\n", pass ? "PASS" : "FAIL", name);
}

static bool writeBytes(const std::wstring &path, const std::vector<unsigned char> &bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(file);
    return ok;
}

// Smallest PE32+ header that carries (or omits) a CLI data directory.
static std::vector<unsigned char> peImage(bool managed)
{
    std::vector<unsigned char> image(1024, 0);
    image[0] = 'M'; image[1] = 'Z';
    image[0x3c] = 0x80;
    memcpy(&image[0x80], "PE\0\0", 4);
    const size_t optional = 0x80 + 24;
    image[optional] = 0x0b; image[optional + 1] = 0x02;
    if (managed) {
        DWORD *cli = reinterpret_cast<DWORD *>(&image[optional + 112 + 14 * 8]);
        cli[0] = 0x2008; cli[1] = 72;
    }
    return image;
}

int main()
{
    using namespace lazer;
    namespace ui = asio::i18n;
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring repo = self;
    repo.resize(repo.find_last_of(L'\\'));                 // tests\bin
    repo.resize(repo.find_last_of(L'\\'));                 // tests
    repo.resize(repo.find_last_of(L'\\'));                 // repository root
    ui::initialize(repo);

    for (const char *code : ui::codes) {
        check(ui::select(code), "catalog selects");
        BackendLine ready = parseBackendLine("[ASIO_READY] " + ui::text("backend.ready", {"42"}));
        BackendLine verified = parseBackendLine(std::string("[Verified Contract] ") + ui::text("contract.verified"));
        BackendLine adaptive = parseBackendLine(std::string("[Compat/Adaptive] ") + ui::text("contract.adaptive"));
        check(ready.ready && verified.contract == ContractKind::Verified && adaptive.contract == ContractKind::Adaptive,
            "fixed markers are recognized whatever the translated wording");
        BackendLine words = parseBackendLine(ui::text("backend.ready", {"42"}));
        check(!words.ready && words.contract == ContractKind::None, "translated display text alone never marks readiness");
    }
    check(!parseBackendLine(" [ASIO_READY] indented").ready && !parseBackendLine("looks ready").ready, "markers must start the line");

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring fixture = std::wstring(temp) + L"LazerAsioWatcherProbe_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    CreateDirectoryW(fixture.c_str(), nullptr);
    for (const wchar_t *name : {L"osu!.exe", L"osu!.dll", L"coreclr.dll", L"hostfxr.dll"}) writeBytes(fixture + name, {'x'});
    writeBytes(fixture + L"osu.Game.dll", peImage(true));
    writeBytes(fixture + L"osu.Framework.dll", peImage(true));
    check(looksLikeLazerClient(fixture + L"osu!.exe"), "lazer layout with managed game/framework images is accepted");
    writeBytes(fixture + L"osu.Framework.dll", peImage(false));
    check(!looksLikeLazerClient(fixture + L"osu!.exe"), "native image named osu.Framework.dll is rejected");
    writeBytes(fixture + L"osu.Framework.dll", {'M', 'Z'});
    check(!looksLikeLazerClient(fixture + L"osu!.exe"), "truncated image is rejected");
    writeBytes(fixture + L"osu.Framework.dll", peImage(true));
    DeleteFileW((fixture + L"hostfxr.dll").c_str());
    check(!looksLikeLazerClient(fixture + L"osu!.exe"), "missing runtime file rejects a stable-style folder");

    check(sanitizeProcessName(L"  osu!.exe ") == L"osu!" && sanitizeProcessName(L"MyLazer.EXE") == L"MyLazer", "process names drop .exe and spaces");
    check(sanitizeProcessName(L"C:\\Games\\osu!.exe").empty() && sanitizeProcessName(L"a|b").empty() && sanitizeProcessName(L" ").empty(),
        "paths and invalid file names are rejected");

    asio::Settings in;
    in.buffer = 64; in.driver = 3; in.rate = 0; in.volume = 7; in.debug = 1; in.contractMode = 0;
    asio::Settings out;
    std::string error;
    check(asio::parseSettings(formatSettings(in), out, error) && out.buffer == 64 && out.driver == 3 && out.rate == 0 &&
        out.volume == 7 && out.debug == 1 && out.contractMode == 0, "written config.ini round-trips all six fields");
    std::wstring atomicTarget = fixture + L"atomic.ini";
    check(writeFileAtomic(atomicTarget, "a") && writeFileAtomic(atomicTarget, "bb") &&
        GetFileAttributesW((atomicTarget + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "atomic writes replace the target and leave no temporary");

    const std::wstring mutexName = L"Local\\LazerAsioWatcherProbe_" + std::to_wstring(GetCurrentProcessId());
    {
        GameWatcher first, second;
        HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ULONGLONG start = GetTickCount64();
        check(first.start(L"LazerAsio-NoSuchGame-55117", [wake] { SetEvent(wake); }, mutexName, 1), "watcher starts with a unique mutex");
        check(!second.start(L"LazerAsio-NoSuchGame-55117", {}, mutexName, 1), "a second watcher cannot run beside the first");
        while (!first.finished() && GetTickCount64() - start < 5000) WaitForSingleObject(wake, 100);
        auto snapshot = first.snapshot();
        check(first.finished() && first.timedOut() && first.result() == 1 && !snapshot.backendRunning, "nonexistent client times out without starting a backend");
        auto log = first.takeLog();
        check(log.size() >= 2 && log[0].find("LazerAsio-NoSuchGame-55117.exe") != std::string::npos, "waiting message names the target executable");
        first.stop();
        check(second.start(L"LazerAsio-NoSuchGame-55117", {}, mutexName, 1), "mutex is released after stop");
        second.stop();
        // No name filter: every process on this machine is checked by its
        // files. A machine without a lazer client must time out, not attach.
        GameWatcher automatic;
        start = GetTickCount64();
        check(automatic.start(L"", [wake] { SetEvent(wake); }, mutexName, 2), "automatic detection starts without an executable name");
        while (!automatic.finished() && GetTickCount64() - start < 8000) WaitForSingleObject(wake, 100);
        auto automaticLog = automatic.takeLog();
        bool attached = false;
        for (auto &line : automaticLog) attached |= line.find("PID=") != std::string::npos;
        check(automatic.finished() && automatic.timedOut() && !attached && !automatic.snapshot().backendRunning,
            "automatic detection ignores non-lazer processes and never starts a backend");
        automatic.stop();
        CloseHandle(wake);
    }

    for (const wchar_t *name : {L"osu!.exe", L"osu!.dll", L"coreclr.dll", L"osu.Game.dll", L"osu.Framework.dll", L"atomic.ini"})
        DeleteFileW((fixture + name).c_str());
    RemoveDirectoryW(fixture.c_str());
    printf("WatcherProbe: checks=%d failures=%d %s\n", checks, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
