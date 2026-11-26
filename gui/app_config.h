#pragma once
// Tool-side files: config.ini (six audio fields shared with the backend),
// gui.ini (window preferences) and the first-use device marker.
#include <windows.h>
#include <string>
#include <cstdio>
#include "../settings.h"
#include "../i18n.h"

namespace lazer {

inline std::wstring toolPath(const wchar_t *name) { return asio::executableSibling(name); }

inline std::string formatSettings(const asio::Settings &s)
{
    char text[256];
    snprintf(text, sizeof(text), "[Config]\nBuffer Size = %d\nASIO Driver ID = %d\nSample Rate = %d\nOutput Volume = %d\nDebug Log = %d\nContract Mode = %d\n",
        s.buffer, s.driver, s.rate, s.volume, s.debug, s.contractMode);
    return text;
}

// Write through a temporary file and an atomic rename, so the backend's
// snapshot reader never observes a partially written configuration.
inline bool writeFileAtomic(const std::wstring &target, const std::string &bytes)
{
    const std::wstring temporary = target + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size();
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

inline bool loadAudioSettings(asio::Settings &result, std::string &error)
{
    return asio::loadSettings(toolPath(L"config.ini"), result, error);
}

inline bool saveAudioSettings(const asio::Settings &settings)
{
    return writeFileAtomic(toolPath(L"config.ini"), formatSettings(settings));
}

// Re-read before a partial update, so a manual edit of another field made
// while the window was open is not overwritten by stale window state.
template <class Update>
inline bool updateAudioSettings(asio::Settings &fallback, Update update)
{
    asio::Settings current;
    std::string error;
    if (!loadAudioSettings(current, error)) current = fallback;
    update(current);
    current.contractMode = 1; // The window always runs the Adaptive contract.
    if (!saveAudioSettings(current)) return false;
    fallback = current;
    return true;
}

inline bool deviceConfigured()
{
    return GetFileAttributesW(toolPath(L".asio-device-configured").c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline bool writeDeviceMarker() { return writeFileAtomic(toolPath(L".asio-device-configured"), "configured\n"); }

inline bool traceMarkerPresent()
{
    return GetFileAttributesW(toolPath(L"playback-trace.enabled").c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline bool setTraceMarker(bool enabled)
{
    const auto path = toolPath(L"playback-trace.enabled");
    if (!enabled) return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    return writeFileAtomic(path, "");
}

// The client is found by its files, so no executable name is stored.
struct Preferences {
    bool autoStart = true;
    bool closeToTray = true;
};

// Optional --watch --process filter for scripted checks.
inline std::wstring sanitizeProcessName(std::wstring name)
{
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
    if (name.empty() || name.size() > 200 || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return {};
    return name;
}

inline Preferences loadPreferences()
{
    Preferences result;
    std::string text;
    if (!asio::i18n::readUtf8(toolPath(L"gui.ini"), text)) return result;
    for (auto &pair : asio::i18n::parseCatalog(text)) {
        const std::string value = asio::i18n::trim(pair.second);
        if (pair.first == "Auto Start") result.autoStart = value != "0";
        else if (pair.first == "Close To Tray") result.closeToTray = value != "0";
    }
    return result;
}

inline bool savePreferences(const Preferences &p)
{
    std::string text = std::string("[Interface]\r\nAuto Start = ") + (p.autoStart ? "1" : "0") +
        "\r\nClose To Tray = " + (p.closeToTray ? "1" : "0") + "\r\n";
    return writeFileAtomic(toolPath(L"gui.ini"), text);
}

} // namespace lazer
