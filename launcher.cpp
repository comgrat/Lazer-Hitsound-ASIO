#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include "i18n.h"
namespace ui=asio::i18n;

// Public standalone watcher. It never starts or copies a game client.
int main(int argc,char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    ui::initialize();
    int languageResult=ui::languageCommand(argc,argv);
    if(languageResult>=0)return languageResult;
    if(argc==2&&!strcmp(argv[1],"--help")){puts(ui::text("launcher.usage").c_str());return 0;}
    if(argc!=1){puts(ui::text("launcher.usage").c_str());return 2;}
    if(!ui::hasSavedLanguage()&&!ui::chooseLanguage())return 2;
    wchar_t executable[MAX_PATH], windows[MAX_PATH];
    DWORD size = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!size || size >= MAX_PATH || !GetWindowsDirectoryW(windows, MAX_PATH)) return 1;
    wchar_t *slash = wcsrchr(executable, L'\\');
    if (!slash) return 1;
    *slash = 0;
    std::wstring root = executable;
    std::wstring powershell = std::wstring(windows) + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring command = L"\"" + powershell + L"\" -NoProfile -ExecutionPolicy Bypass -File \"" + root + L"\\managed\\Start-Standalone.ps1\"";
#ifdef __EXESELECT
    command += L" -SelectProcess";
#endif
    puts(ui::text("launcher.intro").c_str());
    fflush(stdout);
    STARTUPINFOW startup = {}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child = {};
    if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, root.c_str(), &startup, &child))
    {
        puts(ui::text("launcher.failed",{std::to_string(GetLastError())}).c_str());
        getchar(); return 1;
    }
    CloseHandle(child.hThread);
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD result = 1;
    GetExitCodeProcess(child.hProcess, &result);
    CloseHandle(child.hProcess);
    if (result) { puts(ui::text("common.start_failed").c_str());puts(ui::text("common.close").c_str()); getchar(); }
    return int(result);
}
