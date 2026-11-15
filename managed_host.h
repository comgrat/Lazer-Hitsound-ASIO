#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace asio {
// Use the active .NET 8+ host context. Never initialise another runtime.
// https://github.com/dotnet/runtime/blob/main/docs/design/features/native-hosting.md
inline int loadExistingManaged(const wchar_t* assembly, const wchar_t* type,
                               const wchar_t* method, void* argument, int bytes)
{
    HMODULE runtime = nullptr;
    DWORD deadline = GetTickCount() + 5000;
    do {
        runtime = GetModuleHandleW(L"coreclr.dll");
        if (runtime) break;
        Sleep(25);
    } while (LONG(deadline - GetTickCount()) > 0);
    if (!runtime) return int(0x800080a3);
    HMODULE host = GetModuleHandleW(L"hostfxr.dll");
    if (!host) {
        wchar_t path[MAX_PATH];
        DWORD length = GetModuleFileNameW(runtime, path, MAX_PATH);
        if (!length || length >= MAX_PATH) return int(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME));
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash) return int(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME));
        std::wstring hostPath(path, slash + 1);
        hostPath += L"hostfxr.dll";
        host = LoadLibraryExW(hostPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!host) return int(HRESULT_FROM_WIN32(GetLastError()));
        // This component may hold runtime delegates for the process lifetime.
    }
    using get_delegate = int32_t(__cdecl*)(void*, int32_t, void**);
    auto get = reinterpret_cast<get_delegate>(GetProcAddress(host, "hostfxr_get_runtime_delegate"));
    if (!get) return int(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
    using load_component = int32_t(__stdcall*)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, void*, void**);
    load_component load = nullptr;
    int result;
    deadline = GetTickCount() + 5000;
    do {
        result = get(nullptr, 5 /* hdt_load_assembly_and_get_function_pointer */, reinterpret_cast<void**>(&load));
        if (result >= 0 && load) break;
        Sleep(25);
    } while (LONG(deadline - GetTickCount()) > 0);
    if (result < 0 || !load) return result < 0 ? result : E_POINTER;
    using entry_point = int32_t(__stdcall*)(void*, int32_t);
    entry_point entry = nullptr;
    result = load(assembly, type, method, nullptr, nullptr, reinterpret_cast<void**>(&entry));
    if (result < 0 || !entry) return result < 0 ? result : E_POINTER;
    return entry(argument, bytes);
}
}
