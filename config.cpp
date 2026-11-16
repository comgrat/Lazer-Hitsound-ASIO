#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <climits>
#include <cwchar>
#include <windows.h>
#include "fmod/inc/fmod.h"
#include "fmod/inc/fmod_errors.h"
#include "settings.h"
#include "i18n.h"
namespace ui=asio::i18n;

struct Settings
{
    int buffer = 16, driver = 0, rate = 48000, volume = 50, debug = 0, contractMode = 1;
};

static bool parseNumber(const char *text, int &value)
{
    char *end = nullptr;
    errno = 0;
    long number = strtol(text, &end, 10);
    if (end == text || errno || number < INT_MIN || number > INT_MAX) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end) return false;
    value = int(number);
    return true;
}

static Settings readSettings()
{
    Settings settings;
    asio::Settings parsed;
    std::string error;
    std::wstring path=asio::executableSibling(L"config.ini");
    if (asio::loadSettings(path, parsed, error)) {
        settings.buffer=parsed.buffer; settings.driver=parsed.driver; settings.rate=parsed.rate;
        settings.volume=parsed.volume; settings.debug=parsed.debug; settings.contractMode=parsed.contractMode;
    } else if (GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr,"%s\n",ui::text("config.invalid",{error}).c_str());
    }
    return settings;
}

static void showSettings(const Settings &settings)
{
    printf("[Config]\nBuffer Size = %d\nASIO Driver ID = %d\nSample Rate = %d\nOutput Volume = %d\nDebug Log = %d\nContract Mode = %d\n",
           settings.buffer, settings.driver, settings.rate, settings.volume, settings.debug, settings.contractMode);
}

static bool ask(const char *prompt, int current, int minimum, int maximum, int &result)
{
    char line[256];
    for (;;)
    {
        printf("%s [%d]: ", prompt, current);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) return false;
        if (line[0] == '\n' || line[0] == '\r') { result = current; }
        else if (!parseNumber(line, result)) { puts(ui::text("config.integer").c_str()); continue; }
        if (result >= minimum && result <= maximum) return true;
        puts(ui::text("config.range",{std::to_string(minimum),std::to_string(maximum)}).c_str());
    }
}

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    ui::initialize();
    int languageResult=ui::languageCommand(argc,argv);
    if(languageResult>=0)return languageResult;
    if(argc==1&&!ui::hasSavedLanguage()&&!ui::chooseLanguage())return 2;
    struct WaitForConsole
    {
        bool interactive;
        ~WaitForConsole() { if (interactive) { puts(ui::text("common.close").c_str()); getchar(); } }
    } wait{argc == 1};
    // Save next to this executable even when launched from another cwd.
    const auto directory=asio::executableDirectory();
    if (directory.empty() || !SetCurrentDirectoryW(directory.c_str())) return 1;
    Settings settings = readSettings();
    if (argc == 2 && !strcmp(argv[1], "--show")) { showSettings(settings); return 0; }
    bool listOnly = argc == 2 && !strcmp(argv[1], "--list");
    if (argc != 1 && !listOnly) { puts(ui::text("config.usage").c_str()); return 2; }

    FMOD_SYSTEM *system = nullptr;
    FMOD_RESULT result = FMOD_System_Create(&system, FMOD_VERSION);
    if (result != FMOD_OK) { puts(ui::text("config.create_failed",{FMOD_ErrorString(result)}).c_str()); return 1; }
    struct Release { FMOD_SYSTEM *system; ~Release() { if (system) FMOD_System_Release(system); } } release{system};
    result = FMOD_System_SetOutput(system, FMOD_OUTPUTTYPE_ASIO);
    int count = 0;
    if (result == FMOD_OK) result = FMOD_System_GetNumDrivers(system, &count);
    if (result != FMOD_OK) { puts(ui::text("config.enumerate_failed",{FMOD_ErrorString(result)}).c_str()); return 1; }
    puts(ui::text("config.devices").c_str());
    for (int i = 0; i < count; ++i)
    {
        char name[256] = {};
        int rate = 0, channels = 0;
        if (FMOD_System_GetDriverInfo(system, i, name, sizeof(name), nullptr, &rate, nullptr, &channels) == FMOD_OK)
            puts(ui::text("config.device_line",{std::to_string(i),name,std::to_string(rate),std::to_string(channels)}).c_str());
    }
    if (!count) puts(ui::text("config.no_devices").c_str());
    if (listOnly) return 0; // An empty enumeration is successful; no device is opened.
    if (!count) return 1;
    puts(ui::text("config.choose").c_str());
    if (settings.driver < 0 || settings.driver >= count) settings.driver = 0;
    if (!ask(ui::text("config.driver").c_str(), settings.driver, 0, count - 1, settings.driver) ||
        !ask(ui::text("config.buffer").c_str(), settings.buffer, 1, 8192, settings.buffer) ||
        !ask(ui::text("config.rate").c_str(), settings.rate, 0, 768000, settings.rate) ||
        !ask(ui::text("config.volume").c_str(), settings.volume, 0, 100, settings.volume) ||
        !ask(ui::text("config.debug").c_str(), settings.debug, 0, 1, settings.debug) ||
        !ask(ui::text("config.contract_mode").c_str(), settings.contractMode, 0, 1, settings.contractMode)) return 2;
    puts(ui::text("config.checking").c_str());
    if (!settings.rate)
    {
        result = FMOD_System_GetDriverInfo(system, settings.driver, nullptr, 0, nullptr, &settings.rate, nullptr, nullptr);
        if (result != FMOD_OK || settings.rate < 8000) { puts(ui::text("config.default_rate_failed").c_str()); return 1; }
    }
    result = FMOD_System_SetDriver(system, settings.driver);
    if (result == FMOD_OK) result = FMOD_System_SetDSPBufferSize(system, settings.buffer, 2);
    if (result == FMOD_OK) result = FMOD_System_SetSoftwareFormat(system, settings.rate, FMOD_SPEAKERMODE_STEREO, 0);
    if (result == FMOD_OK) result = FMOD_System_Init(system, 32, FMOD_INIT_NORMAL, nullptr);
    if (result != FMOD_OK)
    {
        puts(ui::text("config.check_failed",{FMOD_ErrorString(result)}).c_str());
        return 1;
    }
    FMOD_System_Release(system);
    release.system = nullptr;
    FILE *file = fopen("config.ini.tmp", "w");
    if (!file) { puts(ui::text("config.write_failed").c_str()); return 1; }
    int written = fprintf(file, "[Config]\nBuffer Size = %d\nASIO Driver ID = %d\nSample Rate = %d\nOutput Volume = %d\nDebug Log = %d\nContract Mode = %d\n",
                          settings.buffer, settings.driver, settings.rate, settings.volume, settings.debug, settings.contractMode);
    bool saved = fclose(file) == 0 && written > 0;
    if (!saved || !MoveFileExW(L"config.ini.tmp", L"config.ini", MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    { puts(ui::text("config.save_failed").c_str()); return 1; }
    file = fopen(".asio-device-configured", "w");
    if (!file) { puts(ui::text("config.marker_failed").c_str()); return 1; }
    fputs("configured\n", file);
    fclose(file);
    puts(ui::text("config.saved").c_str());
    showSettings(settings);
    return 0;
}
