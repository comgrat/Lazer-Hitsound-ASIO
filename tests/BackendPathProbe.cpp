// Run the backend's config bootstrap without FMOD init, hooks or a game.
#define main backendEntry
#include "../osu!asio_sound.cpp"
#undef main

int main() {
    ui::initialize();
    const auto directory=asio::executableDirectory();
    if(directory.empty()||!SetCurrentDirectoryW(directory.c_str())||!init())return 1;
    std::wstring cwd(32768,L'\0');DWORD length=GetCurrentDirectoryW(DWORD(cwd.size()),cwd.data());
    if(!length||length>=cwd.size())return 1;
    cwd.resize(length);
    const bool ok=cwd==directory.substr(0,directory.size()-1)&&startupSettings.volume==37&&startupSettings.contractMode==0;
    std::printf("BackendPathProbe %s; exe-side volume=%d mode=%d\n",ok?"PASS":"FAIL",startupSettings.volume,startupSettings.contractMode);
    return ok?0:1;
}
