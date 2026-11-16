#pragma once
#include <windows.h>
#include <cstdio>

// Separate opt-in status mapping; the audio protocol and existing test probes
// keep their layout. No registry, global environment or client files are used.
struct AsioLiveAttach {
    DWORD magic;
    DWORD pid;
    volatile LONG status; // 0=requested, 1=native ready, 2=managed ready, -1=failed
    volatile LONG error;
    LONG contractMode; // Frozen at backend startup: 0=Strict, 1=Adaptive.
    volatile LONG contractStatus; // 0=not validated, 1=ExactMatch, 2=AdaptiveMatch.
};
static_assert(sizeof(AsioLiveAttach)==24, "Managed live-session ABI must match");
constexpr DWORD ASIO_LIVE_MAGIC = 0x41534c32;
inline void asioLiveMapName(char (&name)[80], DWORD pid)
{
    sprintf_s(name, "Local\\OsuAsioLive_%lu", static_cast<unsigned long>(pid));
}
struct AsioLiveMapping {
    HANDLE handle = nullptr;
    AsioLiveAttach* state = nullptr;
    ~AsioLiveMapping() { if (state) UnmapViewOfFile(state); if (handle) CloseHandle(handle); }
    bool create(DWORD pid, int contractMode=1) {
        if(contractMode<0||contractMode>1)return false;
        char name[80]; asioLiveMapName(name, pid);
        handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(AsioLiveAttach), name);
        if (!handle || GetLastError() == ERROR_ALREADY_EXISTS) return false;
        state = static_cast<AsioLiveAttach*>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AsioLiveAttach)));
        if (!state) return false;
        ZeroMemory(state, sizeof(*state)); state->magic = ASIO_LIVE_MAGIC; state->pid = pid; state->contractMode=contractMode;
        return true;
    }
};
