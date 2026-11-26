// LazerASIO.exe: one window for first-use setup, device settings, live ASIO
// volume and the game watcher. Command-line flags keep package checks scriptable.
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <cstdio>
#include <deque>
#include <thread>
#include <mutex>
#include <memory>
#include "ui.h"
#include "app_config.h"
#include "audio_device.h"
#include "watcher.h"
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

namespace i18n = asio::i18n;
using namespace lazer;
using namespace lazer::ui;

static const char *kVersion = "1.3.0";
static const UINT WM_APP_WATCH = WM_APP + 1, WM_APP_DEVICE = WM_APP + 2, WM_APP_TRAY = WM_APP + 3;
static const UINT_PTR TIMER_ANIM = 1, TIMER_TICK = 2;
static const wchar_t *kWindowClass = L"LazerASIOWindow";

static std::wstring W(const std::string &text) { return i18n::widen(text); }
static std::wstring T(const char *key, std::initializer_list<std::string> args = {}) { return W(i18n::text(key, args)); }

static const wchar_t *localeFor(const std::string &code)
{
    if (code == "zh-CN") return L"zh-cn";
    if (code == "zh-TW") return L"zh-tw";
    if (code == "fr") return L"fr-fr";
    if (code == "es") return L"es-es";
    return L"en-us";
}
static int languageIndex()
{
    for (int i = 0; i < 5; ++i) if (i18n::language == i18n::codes[i]) return i;
    return 2;
}

// ---------------------------------------------------------------- app icon

static HICON makeIcon(int size)
{
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header); header.bV5Width = size; header.bV5Height = -size; header.bV5Planes = 1;
    header.bV5BitCount = 32; header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000; header.bV5GreenMask = 0x0000ff00; header.bV5BlueMask = 0x000000ff; header.bV5AlphaMask = 0xff000000;
    void *bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO *>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;
    auto *pixels = static_cast<uint32_t *>(bits);
    const float c = (size - 1) / 2.0f, outer = size * 0.48f, ringOuter = size * 0.34f, ringInner = size * 0.25f;
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        float d = std::hypot(x - c, y - c);
        float disc = std::clamp(outer - d + 0.5f, 0.0f, 1.0f);
        float ring = std::clamp(ringOuter - d + 0.5f, 0.0f, 1.0f) * std::clamp(d - ringInner + 0.5f, 0.0f, 1.0f);
        float r = 255, g = 102 + (255 - 102) * ring, b = 171 + (255 - 171) * ring;
        uint32_t a = uint32_t(disc * 255);
        pixels[y * size + x] = (a << 24) | (uint32_t(r * disc) << 16) | (uint32_t(g * disc) << 8) | uint32_t(b * disc);
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info = {TRUE, 0, 0, mask, color};
    HICON result = CreateIconIndirect(&info);
    DeleteObject(color); DeleteObject(mask);
    return result;
}

// ---------------------------------------------------------------- application

enum class Page { Wizard, Main };
enum class Job { None, Enumerate, Check, Save, Finish, ToneLeft, ToneRight };
enum class Modal { None, Close };

struct JobResult {
    Job job = Job::None;
    FMOD_RESULT result = FMOD_OK;
    std::vector<AsioDevice> devices;
    asio::Settings settings;
};

static const int kBuffers[] = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 2048};
static const int kRates[] = {0, 44100, 48000, 88200, 96000, 176400, 192000};

class App {
public:
    HWND hwnd = nullptr;
    Canvas canvas;
    Gui gui{canvas};

    bool create(HINSTANCE instance, int show);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

private:
    // state
    Page page_ = Page::Main;
    int step_ = 0;
    bool wizardCancelable_ = false;
    Modal modal_ = Modal::None;
    asio::Settings saved_;
    int driver_ = 0, buffer_ = 16, rate_ = 48000;
    int volume_ = 50, volumeBeforeMute_ = 50;
    bool volumeDirty_ = false;
    ULONGLONG volumeChangedAt_ = 0;
    Preferences prefs_;
    std::string bundleMissing_; // files absent from the tool folder; attachment is refused
    bool trace_ = false;
    std::vector<AsioDevice> devices_;
    bool devicesLoaded_ = false;
    std::string deviceError_;
    std::wstring deviceMessage_;
    Color deviceMessageColor_ = theme::foreground1;
    std::thread worker_;
    std::mutex resultLock_;
    JobResult result_;
    Job busy_ = Job::None;
    GameWatcher watcher_;
    WatchSnapshot watch_;
    bool everAttached_ = false;
    std::deque<std::wstring> log_;
    bool advancedOpen_ = false, logOpen_ = true;
    float scroll_ = 0, scrollTarget_ = 0, contentHeight_ = 0;
    float logScroll_ = 0; // lines from the bottom
    float wizardListScroll_ = 0;
    LARGE_INTEGER frequency_ = {}, lastFrame_ = {};
    NOTIFYICONDATAW tray_ = {};
    HICON iconLarge_ = nullptr, iconSmall_ = nullptr;
    bool trackingMouse_ = false;
    UINT taskbarCreated_ = 0;
    WatchPhase notifiedPhase_ = WatchPhase::Stopped;

    // actions
    void loadState();
    void startJob(Job job);
    void finishJob();
    void startWatching();
    void stopWatching();
    void setVolume(int value);
    void flushVolume();
    bool backendBusy() const { return watch_.backendRunning; }
    bool deviceDirty() const { return driver_ != saved_.driver || buffer_ != saved_.buffer || rate_ != saved_.rate || !deviceConfigured(); }
    bool restartNeeded() const
    {
        return watch_.backendRunning && watch_.haveRunning &&
            (watch_.running.driver != saved_.driver || watch_.running.buffer != saved_.buffer ||
             watch_.running.rate != saved_.rate);
    }
    void pullWatch();
    void addLog(const std::string &line);
    void setMessage(const std::wstring &text, Color color) { deviceMessage_ = text; deviceMessageColor_ = color; }
    void openWizard(bool cancelable);
    void exitApp();
    void showWindow();

    // tray
    void addTray();
    void updateTray();
    void trayMenu();
    void balloon(const std::wstring &title, const std::wstring &text);

    // drawing
    void render();
    void drawHeader(float width);
    void drawWizard(const Rect &area);
    void drawMain(const Rect &area);
    float drawStatus(float x, float y, float w);
    float drawVolume(float x, float y, float w);
    float drawDevice(float x, float y, float w);
    float drawAdvanced(float x, float y, float w);
    float drawLog(float x, float y, float w);
    void drawModal();
    Rect card(float x, float y, float w, float h, const std::wstring &title, const wchar_t *glyph);
    void badge(float x, float y, const std::wstring &text, Color color);
    float label(const Rect &r, const std::wstring &text, const std::wstring &hint = L"");
    std::vector<std::wstring> deviceItems(int &selected);
    std::vector<std::wstring> bufferItems(int &selected, std::vector<int> &values);
    std::vector<std::wstring> rateItems(int &selected, std::vector<int> &values);
    std::wstring bufferNote();
    std::wstring statusTitle() const;
    std::wstring statusDetail() const;
    Color statusColor() const;
};

void App::loadState()
{
    std::string error;
    if (!loadAudioSettings(saved_, error)) saved_ = asio::Settings{};
    else if (saved_.contractMode != 1) updateAudioSettings(saved_, [](asio::Settings &) {});
    driver_ = saved_.driver; buffer_ = saved_.buffer; rate_ = saved_.rate;
    volume_ = saved_.volume; volumeBeforeMute_ = volume_ ? volume_ : 50;
    prefs_ = loadPreferences();
    trace_ = traceMarkerPresent();
}

void App::addLog(const std::string &line)
{
    log_.push_back(W(line));
    while (log_.size() > 400) log_.pop_front();
}

void App::pullWatch()
{
    for (auto &line : watcher_.takeLog()) addLog(line);
    watch_ = watcher_.snapshot();
    if (watch_.phase == WatchPhase::Ready) everAttached_ = true;
    if (watcher_.active() && watcher_.finished()) stopWatching();
    if (watch_.phase != notifiedPhase_) {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
            if (watch_.phase == WatchPhase::Ready) balloon(T("gui.status_ready"), statusDetail());
            else if (watch_.phase == WatchPhase::Failed) balloon(T("gui.status_failed"), statusDetail());
        }
        notifiedPhase_ = watch_.phase;
        updateTray();
    }
}

void App::startJob(Job job)
{
    if (busy_ != Job::None) return;
    if (worker_.joinable()) worker_.join();
    busy_ = job;
    asio::Settings request = saved_;
    request.driver = driver_; request.buffer = buffer_; request.rate = rate_;
    if (job == Job::Check || job == Job::Save || job == Job::Finish) setMessage(T("gui.checking"), theme::foreground1);
    if (job == Job::ToneLeft || job == Job::ToneRight) setMessage(T(job == Job::ToneLeft ? "gui.tone_left_playing" : "gui.tone_right_playing"), theme::blue);
    HWND window = hwnd;
    worker_ = std::thread([this, job, request, window] {
        JobResult r;
        r.job = job; r.settings = request;
        switch (job) {
        case Job::Enumerate: r.result = enumerateDevices(r.devices); break;
        case Job::Check: case Job::Save: case Job::Finish: r.result = checkDevice(request.driver, request.buffer, r.settings.rate); break;
        case Job::ToneLeft: case Job::ToneRight: r.result = playTestTone(request.driver, request.buffer, request.rate, job == Job::ToneLeft); break;
        default: break;
        }
        { std::lock_guard<std::mutex> lock(resultLock_); result_ = std::move(r); }
        PostMessageW(window, WM_APP_DEVICE, 0, 0);
    });
}

void App::finishJob()
{
    JobResult r;
    { std::lock_guard<std::mutex> lock(resultLock_); r = std::move(result_); }
    busy_ = Job::None;
    const std::wstring error = W(fmodError(r.result));
    switch (r.job) {
    case Job::Enumerate:
        devicesLoaded_ = true;
        deviceError_ = r.result == FMOD_OK ? std::string() : fmodError(r.result);
        devices_ = std::move(r.devices);
        if (page_ == Page::Wizard && !devices_.empty()) {
            bool found = false;
            for (auto &d : devices_) found |= d.id == driver_;
            if (!found) driver_ = devices_.front().id;
        }
        break;
    case Job::Check:
        if (r.result == FMOD_OK) setMessage(T("gui.check_ok", {std::to_string(r.settings.rate)}), theme::lime);
        else setMessage(T("gui.check_failed", {fmodError(r.result)}), theme::red);
        break;
    case Job::Save: case Job::Finish:
        if (r.result != FMOD_OK) { setMessage(T("gui.check_failed", {fmodError(r.result)}), theme::red); break; }
        {
            asio::Settings settings = r.settings;
            bool ok = updateAudioSettings(saved_, [&](asio::Settings &s) { s.driver = settings.driver; s.buffer = settings.buffer; s.rate = settings.rate; s.volume = volume_; });
            if (ok) ok = writeDeviceMarker();
            if (!ok) { setMessage(T("config.save_failed"), theme::red); break; }
            driver_ = saved_.driver; buffer_ = saved_.buffer; rate_ = saved_.rate;
            volumeDirty_ = false;
            setMessage(T("gui.saved"), theme::lime);
            if (r.job == Job::Finish) {
                if (!i18n::hasSavedLanguage()) i18n::save(i18n::language);
                page_ = Page::Main;
                if (prefs_.autoStart && !watcher_.active()) startWatching();
            }
        }
        break;
    case Job::ToneLeft: case Job::ToneRight:
        if (r.result == FMOD_OK) setMessage(T("gui.tone_done"), theme::lime);
        else setMessage(T("device.play_failed", {fmodError(r.result)}), theme::red);
        break;
    default: break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void App::startWatching()
{
    // An incomplete copy (e.g. no managed\ bridge) would only fail inside the
    // game after injection, so refuse before anything touches the client.
    bundleMissing_.clear();
    for (auto &name : missingBundleFiles()) bundleMissing_ += (bundleMissing_.empty() ? "" : ", ") + name;
    if (!bundleMissing_.empty()) {
        addLog(timestampText() + "  " + i18n::text("gui.bundle_missing", {bundleMissing_}));
        return;
    }
    HWND window = hwnd;
    if (!watcher_.start(L"", [window] { PostMessageW(window, WM_APP_WATCH, 0, 0); })) {
        addLog(timestampText() + "  " + i18n::text("watcher.already_running"));
        return;
    }
    watcher_.setVolume(volume_);
    pullWatch();
}

void App::stopWatching()
{
    watcher_.stop();
    for (auto &line : watcher_.takeLog()) addLog(line);
    watch_ = watcher_.snapshot();
    updateTray();
}

void App::setVolume(int value)
{
    value = std::clamp(value, 0, 100);
    if (value == volume_) return;
    volume_ = value;
    if (value) volumeBeforeMute_ = value;
    watcher_.setVolume(value);
    volumeDirty_ = true;
    volumeChangedAt_ = GetTickCount64();
    updateTray();
}

void App::flushVolume()
{
    if (!volumeDirty_) return;
    int value = volume_;
    if (updateAudioSettings(saved_, [value](asio::Settings &s) { s.volume = value; })) volumeDirty_ = false;
}

void App::openWizard(bool cancelable)
{
    page_ = Page::Wizard;
    step_ = 0;
    wizardCancelable_ = cancelable;
    deviceMessage_.clear();
    driver_ = saved_.driver; buffer_ = saved_.buffer; rate_ = saved_.rate;
    if (!devicesLoaded_) startJob(Job::Enumerate);
}

void App::showWindow()
{
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd);
}

void App::exitApp()
{
    flushVolume();
    watcher_.stop();
    if (worker_.joinable()) worker_.join();
    Shell_NotifyIconW(NIM_DELETE, &tray_);
    DestroyWindow(hwnd);
}

// ---------------------------------------------------------------- tray

void App::addTray()
{
    tray_ = {};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = hwnd;
    tray_.uID = 1;
    tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray_.uCallbackMessage = WM_APP_TRAY;
    tray_.hIcon = iconSmall_;
    wcscpy_s(tray_.szTip, L"LazerASIO");
    Shell_NotifyIconW(NIM_ADD, &tray_);
    updateTray();
}

void App::updateTray()
{
    std::wstring tip = L"LazerASIO · " + statusTitle() + L" · " + std::to_wstring(volume_) + L"%";
    wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE);
    tray_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &tray_);
}

void App::balloon(const std::wstring &title, const std::wstring &text)
{
    tray_.uFlags = NIF_INFO;
    wcsncpy_s(tray_.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(tray_.szInfo, text.c_str(), _TRUNCATE);
    tray_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &tray_);
}

void App::trayMenu()
{
    enum { Show = 1, Up, Down, Mute, Watch, Quit };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (statusTitle() + L" · " + std::to_wstring(volume_) + L"%").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Show, T("gui.tray_show").c_str());
    AppendMenuW(menu, MF_STRING, Up, T("gui.tray_volume_up").c_str());
    AppendMenuW(menu, MF_STRING, Down, T("gui.tray_volume_down").c_str());
    AppendMenuW(menu, MF_STRING, Mute, T(volume_ ? "gui.mute" : "gui.unmute").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (page_ == Page::Wizard ? MF_GRAYED : 0), Watch, T(watcher_.active() ? "gui.stop" : "gui.start").c_str());
    AppendMenuW(menu, MF_STRING, Quit, T("gui.quit").c_str());
    POINT point; GetCursorPos(&point);
    SetForegroundWindow(hwnd);
    int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, point.x, point.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    switch (command) {
    case Show: showWindow(); break;
    case Up: setVolume(volume_ + 5); break;
    case Down: setVolume(volume_ - 5); break;
    case Mute: setVolume(volume_ ? 0 : volumeBeforeMute_); break;
    case Watch: if (watcher_.active()) stopWatching(); else startWatching(); break;
    case Quit: exitApp(); return;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- status text

std::wstring App::statusTitle() const
{
    if (!watcher_.active()) return T(bundleMissing_.empty() ? "gui.status_stopped" : "gui.status_incomplete");
    switch (watch_.phase) {
    case WatchPhase::Waiting: case WatchPhase::Released: return T("gui.status_waiting_auto");
    case WatchPhase::Attaching: return T("gui.status_attaching");
    case WatchPhase::Ready: return T("gui.status_ready");
    case WatchPhase::Failed: return T("gui.status_failed");
    default: return T("gui.status_stopped");
    }
}

std::wstring App::statusDetail() const
{
    const std::string pid = std::to_string(watch_.pid);
    if (!watcher_.active() && !bundleMissing_.empty()) return T("gui.bundle_missing", {bundleMissing_});
    if (!watcher_.active()) return T(everAttached_ ? "gui.status_reattach" : "gui.status_stopped_detail");
    switch (watch_.phase) {
    case WatchPhase::Attaching: return T("gui.status_attaching_detail", {pid});
    case WatchPhase::Ready: return T("gui.status_ready_detail", {pid, wideToUtf8(watch_.gamePath)});
    case WatchPhase::Failed: return T("gui.status_failed_detail", {std::to_string(watch_.exitCode)});
    default: return T("gui.status_waiting_detail");
    }
}

Color App::statusColor() const
{
    if (!watcher_.active()) return bundleMissing_.empty() ? theme::foreground1 : theme::red;
    switch (watch_.phase) {
    case WatchPhase::Attaching: return theme::blue;
    case WatchPhase::Ready: return theme::lime;
    case WatchPhase::Failed: return theme::red;
    default: return theme::yellow;
    }
}

// ---------------------------------------------------------------- drawing helpers

Rect App::card(float x, float y, float w, float h, const std::wstring &title, const wchar_t *glyph)
{
    canvas.fill({x, y + 2, w, h}, hex(0x000000, 0.18f), 12);
    canvas.fill({x, y, w, h}, theme::background4, 12);
    if (!title.empty()) {
        TextStyle i = style(16, theme::pink, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
        i.font = Font::Icon;
        canvas.text(glyph, {x + 18, y + 16, 22, 24}, i);
        canvas.text(title, {x + 48, y + 16, w - 220, 24}, style(16, theme::text, DWRITE_FONT_WEIGHT_SEMI_BOLD));
    }
    return {x + 20, y + 56, w - 40, h - 72};
}

void App::badge(float right, float y, const std::wstring &text, Color color)
{
    TextStyle s = style(12, color, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
    float w = canvas.measure(text, s).width + 22;
    Rect r = {right - w, y, w, 24};
    canvas.fill(r, alpha(color, 0.14f), 12);
    canvas.stroke(r, alpha(color, 0.7f), 1, 12);
    canvas.text(text, r, s);
}

float App::label(const Rect &r, const std::wstring &text, const std::wstring &hint)
{
    if (hint.empty()) { canvas.text(text, r, style(14, theme::content2)); return r.h; }
    canvas.text(text, {r.x, r.y, r.w, 20}, style(14, theme::content2));
    canvas.text(hint, {r.x, r.y + 20, r.w, 18}, style(12, theme::foreground1));
    return r.h;
}

std::vector<std::wstring> App::deviceItems(int &selected)
{
    std::vector<std::wstring> items;
    selected = -1;
    for (size_t i = 0; i < devices_.size(); ++i) {
        auto &d = devices_[i];
        items.push_back(L"[" + std::to_wstring(d.id) + L"] " + W(d.name) + L"  ·  " + std::to_wstring(d.rate) + L" Hz");
        if (d.id == driver_) selected = int(i);
    }
    if (selected < 0) {
        items.push_back(L"[" + std::to_wstring(driver_) + L"] " + T(devicesLoaded_ ? "gui.device_missing" : "gui.loading"));
        selected = int(items.size()) - 1;
    }
    return items;
}

std::vector<std::wstring> App::bufferItems(int &selected, std::vector<int> &values)
{
    values.assign(std::begin(kBuffers), std::end(kBuffers));
    if (std::find(values.begin(), values.end(), buffer_) == values.end()) { values.push_back(buffer_); std::sort(values.begin(), values.end()); }
    std::vector<std::wstring> items;
    for (size_t i = 0; i < values.size(); ++i) {
        items.push_back(T("gui.buffer_value", {std::to_string(values[i])}));
        if (values[i] == buffer_) selected = int(i);
    }
    return items;
}

std::vector<std::wstring> App::rateItems(int &selected, std::vector<int> &values)
{
    values.assign(std::begin(kRates), std::end(kRates));
    if (std::find(values.begin(), values.end(), rate_) == values.end()) { values.push_back(rate_); std::sort(values.begin(), values.end()); }
    std::vector<std::wstring> items;
    for (size_t i = 0; i < values.size(); ++i) {
        items.push_back(values[i] ? std::to_wstring(values[i]) + L" Hz" : T("gui.rate_default"));
        if (values[i] == rate_) selected = int(i);
    }
    return items;
}

std::wstring App::bufferNote()
{
    int rate = rate_;
    if (!rate) for (auto &d : devices_) if (d.id == driver_) rate = d.rate;
    if (rate <= 0) return T("gui.buffer_note_unknown");
    char ms[32];
    snprintf(ms, sizeof(ms), "%.2f", buffer_ * 2 * 1000.0 / rate);
    return T("gui.buffer_note", {ms, std::to_string(buffer_), std::to_string(rate)});
}

// ---------------------------------------------------------------- header

void App::drawHeader(float width)
{
    const float h = 68;
    canvas.fill({0, 0, width, h}, theme::background6);
    canvas.gradient({0, h - 2, width, 2}, theme::pink, theme::purple);
    canvas.circle(38, h / 2, 17, theme::pink);
    canvas.ring(38, h / 2, 9.5f, theme::text, 3);
    canvas.text(L"LazerASIO", {66, 12, 200, 26}, style(20, theme::text, DWRITE_FONT_WEIGHT_BOLD));
    float titleW = canvas.measure(L"LazerASIO", style(20, theme::text, DWRITE_FONT_WEIGHT_BOLD)).width;
    canvas.text(W(std::string("v") + kVersion), {66 + titleW + 8, 16, 140, 22}, style(12, theme::foreground1));
    canvas.text(T("gui.tagline"), {66, 38, std::max(0.0f, width - 66 - 230), 18}, style(12.5f, theme::foreground1));

    std::vector<std::wstring> names;
    for (auto name : i18n::names) names.push_back(W(name));
    int selected = languageIndex();
    TextStyle globe = style(14, theme::light1, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
    globe.font = Font::Icon;
    canvas.text(icon::globe, {width - 214, 17, 24, 34}, globe);
    if (gui.dropdown(id("language"), {width - 186, 17, 166, 34}, names, selected) && i18n::save(i18n::codes[selected])) {
        canvas.setLocale(localeFor(i18n::language));
        updateTray();
    }
}

// ---------------------------------------------------------------- wizard

void App::drawWizard(const Rect &area)
{
    const float w = std::min(640.0f, area.w - 48), x = area.x + (area.w - w) / 2;
    float y = area.y + 28;
    // step indicator
    const char *steps[] = {"gui.step_language", "gui.step_device", "gui.step_audio", "gui.step_volume"};
    float segment = (w - 3 * 8) / 4;
    for (int i = 0; i < 4; ++i) {
        Rect bar = {x + i * (segment + 8), y, segment, 4};
        float on = gui.anim(id("wizard-step", uint64_t(i)), i <= step_ ? 1.0f : 0.0f, 12);
        canvas.fill(bar, mix(theme::background3, theme::pink, on), 2);
        canvas.text(T(steps[i]), {bar.x, y + 10, segment, 18}, style(12, i == step_ ? theme::pink : theme::foreground1, i == step_ ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL));
    }
    y += 44;
    const float cardH = std::clamp(area.bottom() - y - 24, 420.0f, 560.0f);
    canvas.fill({x, y + 2, w, cardH}, hex(0x000000, 0.18f), 14);
    canvas.fill({x, y, w, cardH}, theme::background4, 14);
    Rect body = {x + 28, y + 26, w - 56, cardH - 26 - 84};
    std::wstring titleText = T(step_ == 0 ? "gui.wizard_language" : step_ == 1 ? "gui.wizard_device" : step_ == 2 ? "gui.wizard_audio" : "gui.wizard_volume");
    canvas.text(titleText, {body.x, body.y, body.w, 30}, style(22, theme::text, DWRITE_FONT_WEIGHT_BOLD));
    if (step_ == 0) canvas.text(L"Choose language · 选择语言 · 選擇語言 · Langue · Idioma", {body.x, body.y + 32, body.w, 20}, style(13, theme::foreground1));
    float cy = body.y + 64;
    bool canNext = true;

    if (step_ == 0) {
        float cellW = (body.w - 12) / 2;
        for (int i = 0; i < 5; ++i) {
            Rect cell = {body.x + (i % 2) * (cellW + 12), cy + (i / 2) * 62, cellW, 52};
            bool on = i == languageIndex();
            if (gui.selectable(id("wizard-language", uint64_t(i)), cell, on) && i18n::save(i18n::codes[i])) {
                canvas.setLocale(localeFor(i18n::language));
                updateTray();
            }
            canvas.text(W(i18n::names[i]), {cell.x + 18, cell.y, cell.w - 80, cell.h}, style(16, on ? theme::text : theme::content2, DWRITE_FONT_WEIGHT_SEMI_BOLD));
            canvas.text(W(i18n::codes[i]), {cell.right() - 76, cell.y, 60, cell.h}, style(12, on ? theme::pink : theme::foreground1, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
        }
    } else if (step_ == 1) {
        canvas.text(T("gui.wizard_device_hint"), {body.x, body.y + 32, body.w, 20}, style(13, theme::foreground1));
        Rect list = {body.x, cy, body.w, body.bottom() - cy - 104};
        canNext = devicesLoaded_ && !devices_.empty();
        if (!devicesLoaded_) canvas.text(T("gui.loading"), list, style(14, theme::foreground1, DWRITE_FONT_WEIGHT_NORMAL, Align::Center));
        else if (devices_.empty()) {
            TextStyle s = style(14, deviceError_.empty() ? theme::yellow : theme::red, DWRITE_FONT_WEIGHT_NORMAL, Align::Center); s.wrap = true;
            canvas.text(deviceError_.empty() ? T("config.no_devices") : T("config.enumerate_failed", {deviceError_}), list.inset(20, 0), s);
        } else {
            const float row = 58;
            float maxScroll = std::max(0.0f, devices_.size() * row - list.h);
            if (gui.hovering(list) && gui.in.wheel != 0) { wizardListScroll_ = std::clamp(wizardListScroll_ - gui.in.wheel * row, 0.0f, maxScroll); gui.in.wheel = 0; }
            canvas.pushClip(list);
            for (size_t i = 0; i < devices_.size(); ++i) {
                auto &d = devices_[i];
                Rect cell = {list.x, list.y + i * row - wizardListScroll_, list.w, row - 8};
                bool on = d.id == driver_;
                if (gui.selectable(id("wizard-device", uint64_t(i)), cell, on, busy_ == Job::None)) driver_ = d.id;
                canvas.text(L"[" + std::to_wstring(d.id) + L"]", {cell.x + 16, cell.y, 40, cell.h}, style(14, on ? theme::pink : theme::foreground1, DWRITE_FONT_WEIGHT_SEMI_BOLD));
                canvas.text(W(d.name), {cell.x + 56, cell.y + 6, cell.w - 72, 22}, style(15, theme::text, DWRITE_FONT_WEIGHT_SEMI_BOLD));
                canvas.text(T("gui.device_caps", {std::to_string(d.rate), std::to_string(d.channels)}), {cell.x + 56, cell.y + 27, cell.w - 72, 18}, style(12.5f, theme::foreground1));
            }
            canvas.popClip();
        }
        float by = body.bottom() - 92;
        bool idle = busy_ == Job::None && !backendBusy() && !devices_.empty();
        if (gui.button(id("wizard-refresh"), {body.x, by, 120, 38}, T("gui.refresh"), ButtonKind::Ghost, busy_ == Job::None, icon::refresh)) { devicesLoaded_ = false; startJob(Job::Enumerate); }
        if (gui.button(id("wizard-left"), {body.x + 132, by, 150, 38}, T("gui.tone_left"), ButtonKind::Secondary, idle, icon::headphone)) startJob(Job::ToneLeft);
        if (gui.button(id("wizard-right"), {body.x + 294, by, 150, 38}, T("gui.tone_right"), ButtonKind::Secondary, idle, icon::headphone)) startJob(Job::ToneRight);
        TextStyle warn = style(12.5f, theme::yellow); warn.wrap = true; warn.top = true;
        canvas.text(deviceMessage_.empty() ? T("gui.tone_warning") : deviceMessage_, {body.x, by + 48, body.w, 40}, deviceMessage_.empty() ? warn : style(12.5f, deviceMessageColor_));
    } else if (step_ == 2) {
        std::vector<int> buffers, rates;
        int bufferSel = 0, rateSel = 0;
        auto bufferList = bufferItems(bufferSel, buffers);
        auto rateList = rateItems(rateSel, rates);
        label({body.x, cy, 180, 40}, T("gui.buffer"));
        if (gui.dropdown(id("wizard-buffer"), {body.x + 190, cy, 220, 40}, bufferList, bufferSel)) { buffer_ = buffers[size_t(bufferSel)]; deviceMessage_.clear(); }
        cy += 56;
        label({body.x, cy, 180, 40}, T("gui.rate"));
        if (gui.dropdown(id("wizard-rate"), {body.x + 190, cy, 220, 40}, rateList, rateSel)) { rate_ = rates[size_t(rateSel)]; deviceMessage_.clear(); }
        cy += 60;
        canvas.fill({body.x, cy, body.w, 1}, theme::background2);
        cy += 16;
        canvas.text(bufferNote(), {body.x, cy, body.w, 22}, style(14, theme::content2));
        canvas.text(T("gui.latency_note"), {body.x, cy + 24, body.w, 20}, style(12.5f, theme::foreground1));
        cy += 64;
        bool idle = busy_ == Job::None && !backendBusy();
        if (gui.button(id("wizard-check"), {body.x, cy, 220, 40}, T("gui.check"), ButtonKind::Secondary, idle, icon::check)) startJob(Job::Check);
        if (!deviceMessage_.empty()) canvas.text(deviceMessage_, {body.x + 236, cy, body.w - 236, 40}, style(13, deviceMessageColor_));
        else if (backendBusy()) canvas.text(T("gui.device_busy"), {body.x + 236, cy, body.w - 236, 40}, style(13, theme::yellow));
    } else {
        canvas.text(std::to_wstring(volume_), {body.x, cy, body.w - 30, 56}, style(44, theme::text, DWRITE_FONT_WEIGHT_BOLD, Align::Right));
        canvas.text(L"%", {body.right() - 26, cy + 14, 26, 40}, style(18, theme::foreground1));
        int value = volume_;
        if (gui.slider(id("wizard-volume"), {body.x, cy + 62, body.w, 36}, value, 0, 100)) setVolume(value);
        cy += 118;
        TextStyle note = style(13, theme::content2); note.wrap = true; note.top = true;
        float h1 = canvas.measure(T("gui.volume_note"), note, body.w).height;
        canvas.text(T("gui.volume_note"), {body.x + 26, cy, body.w - 26, h1 + 4}, note);
        TextStyle i = style(14, theme::blue); i.font = Font::Icon; i.top = true;
        canvas.text(icon::info, {body.x, cy + 1, 20, 20}, i);
        cy += h1 + 14;
        float h2 = canvas.measure(T("gui.hook_note"), note, body.w - 26).height;
        canvas.text(T("gui.hook_note"), {body.x + 26, cy, body.w - 26, h2 + 4}, note);
        i.color = theme::yellow;
        canvas.text(icon::warning, {body.x, cy + 1, 20, 20}, i);
        if (!deviceMessage_.empty()) canvas.text(deviceMessage_, {body.x, body.bottom() - 26, body.w, 24}, style(13, deviceMessageColor_));
    }

    // navigation
    float ny = y + cardH - 64;
    canvas.fill({x + 1, ny - 12, w - 2, 1}, theme::background3);
    if (step_ > 0 && gui.button(id("wizard-back"), {x + 28, ny, 130, 40}, T("gui.back"), ButtonKind::Ghost, busy_ == Job::None, icon::back)) { --step_; deviceMessage_.clear(); }
    if (step_ == 0 && wizardCancelable_ && gui.button(id("wizard-cancel"), {x + 28, ny, 130, 40}, T("gui.cancel"), ButtonKind::Ghost)) {
        page_ = Page::Main; driver_ = saved_.driver; buffer_ = saved_.buffer; rate_ = saved_.rate; deviceMessage_.clear();
    }
    if (step_ < 3) {
        if (gui.button(id("wizard-next"), {x + w - 28 - 150, ny, 150, 40}, T("gui.next"), ButtonKind::Primary, canNext && busy_ == Job::None, icon::forward)) {
            ++step_; deviceMessage_.clear();
            if (step_ == 1 && !devicesLoaded_) startJob(Job::Enumerate);
        }
    } else if (gui.button(id("wizard-finish"), {x + w - 28 - 170, ny, 170, 40}, T(busy_ == Job::Finish ? "gui.checking_short" : "gui.finish"), ButtonKind::Primary, busy_ == Job::None, icon::check)) {
        if (backendBusy()) {
            bool ok = updateAudioSettings(saved_, [&](asio::Settings &s) { s.driver = driver_; s.buffer = buffer_; s.rate = rate_; s.volume = volume_; }) && writeDeviceMarker();
            if (ok) { if (!i18n::hasSavedLanguage()) i18n::save(i18n::language); volumeDirty_ = false; page_ = Page::Main; setMessage(T("gui.saved_busy"), theme::yellow); }
            else setMessage(T("config.save_failed"), theme::red);
        } else startJob(Job::Finish);
    }
}

// ---------------------------------------------------------------- main page

float App::drawStatus(float x, float y, float w)
{
    const float h = 132;
    card(x, y, w, h, L"", nullptr);
    Color color = statusColor();
    float pulse = 0;
    if (watcher_.active() && (watch_.phase == WatchPhase::Waiting || watch_.phase == WatchPhase::Attaching)) {
        pulse = float(GetTickCount64() % 1600) / 1600.0f;
        gui.wantsFrame = true;
        canvas.circle(x + 32, y + 36, 7 + 10 * pulse, alpha(color, 0.35f * (1 - pulse)));
    }
    canvas.circle(x + 32, y + 36, 7, color);
    std::wstring title = statusTitle();
    TextStyle big = style(21, theme::text, DWRITE_FONT_WEIGHT_BOLD);
    canvas.text(title, {x + 52, y + 20, w - 260, 32}, big);
    float titleW = std::min(canvas.measure(title, big).width, w - 260);
    if (watcher_.active() && watch_.contract != ContractKind::None)
        badge(x + 52 + titleW + 12 + canvas.measure(watch_.contract == ContractKind::Verified ? L"Verified Contract" : L"Compat/Adaptive", style(12, theme::text, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 22, y + 24,
            watch_.contract == ContractKind::Verified ? L"Verified Contract" : L"Compat/Adaptive", watch_.contract == ContractKind::Verified ? theme::lime : theme::yellow);
    canvas.text(statusDetail(), {x + 52, y + 56, w - 250, 20}, style(13.5f, theme::content2));
    std::wstring summary;
    if (watch_.backendRunning && !watch_.deviceLine.empty()) summary = W(watch_.deviceLine);
    else {
        std::wstring name = T("gui.device_missing");
        for (auto &d : devices_) if (d.id == saved_.driver) name = W(d.name);
        summary = T("gui.config_summary", {std::to_string(saved_.driver), wideToUtf8(name), saved_.rate ? std::to_string(saved_.rate) + " Hz" : i18n::text("gui.rate_default"), std::to_string(saved_.buffer)});
    }
    canvas.text(summary, {x + 52, y + 82, w - 250, 20}, style(12.5f, theme::foreground1));

    Rect button = {x + w - 20 - 164, y + (h - 44) / 2, 164, 44};
    if (watcher_.active()) {
        if (gui.button(id("watch-stop"), button, T("gui.stop"), ButtonKind::Danger, true, icon::stop)) stopWatching();
    } else if (gui.button(id("watch-start"), button, T("gui.start"), ButtonKind::Primary, true, icon::play)) startWatching();
    if (watch_.phase == WatchPhase::Failed && !watch_.stderrLog.empty() &&
        gui.button(id("watch-log"), {button.x - 132, button.y, 120, 44}, T("gui.view_log"), ButtonKind::Ghost, true, icon::document))
        ShellExecuteW(hwnd, L"open", watch_.stderrLog.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return h;
}

float App::drawVolume(float x, float y, float w)
{
    const float h = 136;
    Rect body = card(x, y, w, h, T("gui.volume_title"), icon::volume);
    TextStyle big = style(30, volume_ ? theme::text : theme::foreground1, DWRITE_FONT_WEIGHT_BOLD, Align::Right);
    canvas.text(std::to_wstring(volume_), {x + w - 150, y + 10, 110, 40}, big);
    canvas.text(L"%", {x + w - 40, y + 20, 24, 30}, style(15, theme::foreground1));
    if (gui.iconButton(id("mute"), {body.x - 4, body.y, 40, 40}, volume_ ? icon::volume : icon::mute, true, volume_ ? theme::content2 : theme::red))
        setVolume(volume_ ? 0 : volumeBeforeMute_);
    int value = volume_;
    if (gui.slider(id("volume"), {body.x + 48, body.y + 2, body.w - 48, 36}, value, 0, 100)) setVolume(value);
    float hy = body.y + 46;
    if (watch_.backendRunning) {
        canvas.circle(body.x + 6, hy + 9, 4, theme::lime);
        canvas.text(T("gui.volume_live"), {body.x + 18, hy, body.w - 18, 18}, style(12.5f, theme::lime));
    } else canvas.text(T(volume_ ? "gui.volume_saved" : "gui.volume_muted"), {body.x, hy, body.w, 18}, style(12.5f, theme::foreground1));
    return h;
}

float App::drawDevice(float x, float y, float w)
{
    const float rowH = 40, gap = 14, labelW = std::min(190.0f, w * 0.3f);
    float h = 56 + (rowH + gap) * 3 + 30 + 52 + (deviceMessage_.empty() ? 0 : 28) + 16;
    Rect body = card(x, y, w, h, T("gui.device_title"), icon::speaker);
    if (restartNeeded()) badge(x + w - 20, y + 16, T("gui.restart_needed"), theme::yellow);
    else if (deviceDirty() && deviceConfigured()) badge(x + w - 20, y + 16, T("gui.unsaved"), theme::blue);
    float cy = body.y;
    float controlX = body.x + labelW, controlW = body.w - labelW;

    int deviceSel = 0;
    auto devices = deviceItems(deviceSel);
    label({body.x, cy, labelW, rowH}, T("gui.device"));
    if (gui.dropdown(id("device"), {controlX, cy, controlW - 52, rowH}, devices, deviceSel, busy_ == Job::None) && deviceSel < int(devices_.size()))
        driver_ = devices_[size_t(deviceSel)].id;
    if (gui.iconButton(id("device-refresh"), {controlX + controlW - 42, cy, 40, 40}, icon::refresh, busy_ == Job::None)) { devicesLoaded_ = false; startJob(Job::Enumerate); }
    cy += rowH + gap;

    std::vector<int> buffers, rates;
    int bufferSel = 0, rateSel = 0;
    auto bufferList = bufferItems(bufferSel, buffers);
    auto rateList = rateItems(rateSel, rates);
    label({body.x, cy, labelW, rowH}, T("gui.buffer"));
    if (gui.dropdown(id("buffer"), {controlX, cy, std::min(240.0f, controlW), rowH}, bufferList, bufferSel)) buffer_ = buffers[size_t(bufferSel)];
    cy += rowH + gap;
    label({body.x, cy, labelW, rowH}, T("gui.rate"));
    if (gui.dropdown(id("rate"), {controlX, cy, std::min(240.0f, controlW), rowH}, rateList, rateSel)) rate_ = rates[size_t(rateSel)];
    cy += rowH + gap;
    canvas.text(bufferNote() + L"  ·  " + T("gui.latency_short"), {controlX, cy - 6, controlW, 22}, style(12.5f, theme::foreground1));
    cy += 30;

    bool idle = busy_ == Job::None && !backendBusy();
    if (gui.button(id("tone-left"), {body.x, cy, 128, 38}, T("gui.tone_left"), ButtonKind::Secondary, idle && !devices_.empty(), icon::headphone)) startJob(Job::ToneLeft);
    if (gui.button(id("tone-right"), {body.x + 138, cy, 128, 38}, T("gui.tone_right"), ButtonKind::Secondary, idle && !devices_.empty(), icon::headphone)) startJob(Job::ToneRight);
    float saveW = 170;
    if (gui.button(id("device-save"), {body.right() - saveW, cy, saveW, 38}, T(backendBusy() ? "gui.save" : busy_ == Job::Save ? "gui.checking_short" : "gui.check_save"),
            ButtonKind::Primary, busy_ == Job::None && deviceDirty())) {
        if (backendBusy()) {
            bool ok = updateAudioSettings(saved_, [&](asio::Settings &s) { s.driver = driver_; s.buffer = buffer_; s.rate = rate_; }) && writeDeviceMarker();
            setMessage(T(ok ? "gui.saved_busy" : "config.save_failed"), ok ? theme::yellow : theme::red);
        } else startJob(Job::Save);
    }
    if (gui.button(id("device-revert"), {body.right() - saveW - 110, cy, 100, 38}, T("gui.revert"), ButtonKind::Ghost, busy_ == Job::None && deviceDirty() && deviceConfigured())) {
        driver_ = saved_.driver; buffer_ = saved_.buffer; rate_ = saved_.rate; deviceMessage_.clear();
    }
    cy += 48;
    if (backendBusy() && deviceMessage_.empty()) canvas.text(T("gui.device_busy"), {body.x, cy, body.w, 20}, style(12.5f, theme::foreground1));
    if (!deviceMessage_.empty()) canvas.text(deviceMessage_, {body.x, cy, body.w, 22}, style(13, deviceMessageColor_));
    return h;
}

float App::drawAdvanced(float x, float y, float w)
{
    const float headerH = 60;
    float open = gui.anim(id("advanced-open"), advancedOpen_ ? 1.0f : 0.0f, 14);
    const float rowH = 48;
    const float fullH = headerH + 4 + rowH * 4 + 16 + 56;
    float h = headerH + (fullH - headerH) * open;
    card(x, y, w, h, T("gui.advanced_title"), icon::settings);
    Rect header = {x, y, w, headerH};
    TextStyle chevron = style(13, theme::light1, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
    chevron.font = Font::Icon;
    canvas.text(advancedOpen_ ? icon::chevronUp : icon::chevronDown, {x + w - 50, y + 16, 30, 26}, chevron);
    if (gui.clicked(id("advanced-header"), header)) advancedOpen_ = !advancedOpen_;
    if (open < 0.01f) return h;
    canvas.pushClip({x, y + headerH, w, h - headerH});
    float bx = x + 20, bw = w - 40, cy = y + headerH + 4;

    struct ToggleRow { const char *key, *title, *hint; bool value; };
    ToggleRow rows[] = {
        {"debug", "gui.debug", "gui.debug_hint", saved_.debug != 0},
        {"trace", "gui.trace", "gui.trace_hint", trace_},
        {"autostart", "gui.autostart", "gui.autostart_hint", prefs_.autoStart},
        {"tray", "gui.close_to_tray", "gui.close_to_tray_hint", prefs_.closeToTray},
    };
    for (auto &row : rows) {
        label({bx, cy, bw - 70, 40}, T(row.title), T(row.hint));
        bool value = row.value;
        if (gui.toggle(id(row.key), {bx + bw - 44, cy, 44, 40}, value)) {
            std::string key = row.key;
            if (key == "debug") updateAudioSettings(saved_, [value](asio::Settings &s) { s.debug = value ? 1 : 0; });
            else if (key == "trace") { if (setTraceMarker(value)) trace_ = value; }
            else if (key == "autostart") { prefs_.autoStart = value; savePreferences(prefs_); }
            else { prefs_.closeToTray = value; savePreferences(prefs_); }
        }
        cy += rowH;
    }
    cy += 16;
    float bwid = (bw - 24) / 3;
    if (gui.button(id("open-logs"), {bx, cy, bwid, 38}, T("gui.open_logs"), ButtonKind::Ghost, true, icon::folder)) {
        auto logs = asio::executableSibling(L"logs");
        CreateDirectoryW(logs.c_str(), nullptr);
        ShellExecuteW(hwnd, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (gui.button(id("open-config"), {bx + bwid + 12, cy, bwid, 38}, T("gui.open_config"), ButtonKind::Ghost, true, icon::document))
        ShellExecuteW(hwnd, L"open", L"notepad.exe", (L"\"" + toolPath(L"config.ini") + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    if (gui.button(id("rerun-wizard"), {bx + 2 * (bwid + 12), cy, bwid, 38}, T("gui.rerun_wizard"), ButtonKind::Ghost, busy_ == Job::None, icon::wizard)) openWizard(true);
    canvas.popClip();
    return h;
}

float App::drawLog(float x, float y, float w)
{
    const float headerH = 60, boxH = 236;
    float open = gui.anim(id("log-open"), logOpen_ ? 1.0f : 0.0f, 14);
    float h = headerH + (boxH + 20) * open;
    card(x, y, w, h, T("gui.log_title"), icon::list);
    TextStyle chevron = style(13, theme::light1, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
    chevron.font = Font::Icon;
    canvas.text(logOpen_ ? icon::chevronUp : icon::chevronDown, {x + w - 50, y + 16, 30, 26}, chevron);
    if (logOpen_ && gui.button(id("log-clear"), {x + w - 150, y + 14, 90, 32}, T("gui.clear"), ButtonKind::Ghost, !log_.empty())) { log_.clear(); logScroll_ = 0; }
    else if (gui.clicked(id("log-header"), {x, y, w - 160, headerH})) logOpen_ = !logOpen_;
    if (open < 0.01f) return h;
    Rect box = {x + 20, y + headerH, w - 40, boxH * open};
    canvas.fill(box, theme::background6, 8);
    TextStyle mono = style(12, theme::content2); mono.font = Font::Mono;
    const float line = 18;
    int visible = int((boxH - 16) / line);
    int maxScroll = std::max(0, int(log_.size()) - visible);
    if (gui.hovering(box) && gui.in.wheel != 0) { logScroll_ = std::clamp(logScroll_ + gui.in.wheel * 3, 0.0f, float(maxScroll)); gui.in.wheel = 0; }
    logScroll_ = std::min(logScroll_, float(maxScroll));
    canvas.pushClip(box.inset(8));
    int first = std::max(0, int(log_.size()) - visible - int(logScroll_));
    for (int i = 0; i < visible && first + i < int(log_.size()); ++i) {
        const std::wstring &text = log_[size_t(first + i)];
        Color color = theme::content2;
        if (text.find(L"[ASIO_READY]") != std::wstring::npos || text.find(L"[Verified Contract]") != std::wstring::npos) color = theme::lime;
        else if (text.find(L"[Compat/Adaptive]") != std::wstring::npos) color = theme::yellow;
        mono.color = color;
        canvas.text(text, {box.x + 12, box.y + 8 + i * line, box.w - 24, line}, mono);
    }
    if (log_.empty()) canvas.text(T("gui.log_empty"), box, style(13, theme::foreground1, DWRITE_FONT_WEIGHT_NORMAL, Align::Center));
    canvas.popClip();
    return h;
}

void App::drawMain(const Rect &area)
{
    const float w = std::min(780.0f, area.w - 40), x = area.x + (area.w - w) / 2;
    float maxScroll = std::max(0.0f, contentHeight_ - area.h);
    scrollTarget_ = std::clamp(scrollTarget_, 0.0f, maxScroll);
    scroll_ = gui.anim(id("page-scroll"), scrollTarget_, 18);
    canvas.pushClip(area);
    float y = area.y + 20 - scroll_;
    const float start = y;
    y += drawStatus(x, y, w) + 16;
    y += drawVolume(x, y, w) + 16;
    y += drawDevice(x, y, w) + 16;
    y += drawAdvanced(x, y, w) + 16;
    y += drawLog(x, y, w) + 24;
    contentHeight_ = y - start + 20;
    canvas.popClip();
    // After the cards, so the log box keeps wheel input it consumed itself.
    if (!gui.overPopup() && !gui.modal && gui.in.wheel != 0 && area.contains(gui.in.x, gui.in.y)) {
        scrollTarget_ = std::clamp(scrollTarget_ - gui.in.wheel * 80, 0.0f, std::max(0.0f, contentHeight_ - area.h));
        gui.in.wheel = 0;
        gui.wantsFrame = true;
    }
    if (maxScroll > 0) {
        float trackH = area.h - 16, thumbH = std::max(40.0f, trackH * area.h / contentHeight_);
        float thumbY = area.y + 8 + (trackH - thumbH) * (scroll_ / maxScroll);
        canvas.fill({area.right() - 8, thumbY, 4, thumbH}, alpha(theme::background1, 0.8f), 2);
    }
}

void App::drawModal()
{
    float W0 = canvas.width(), H0 = canvas.height();
    float open = gui.anim(id("modal"), 1.0f, 18);
    canvas.fill({0, 0, W0, H0}, hex(0x000000, 0.55f * open));
    const float w = std::min(500.0f, W0 - 40);
    TextStyle body = style(14, theme::content2); body.wrap = true; body.top = true;
    std::wstring text = T(watch_.backendRunning ? "gui.close_attached" : "gui.close_waiting");
    float textH = canvas.measure(text, body, w - 56).height;
    const float h = 78 + textH + 92;
    Rect r = {(W0 - w) / 2, (H0 - h) / 2 + 12 * (1 - open), w, h};
    canvas.fill({r.x, r.y + 4, r.w, r.h}, hex(0x000000, 0.35f), 14);
    canvas.fill(r, theme::background4, 14);
    canvas.fill({r.x + 16, r.y, r.w - 32, 3}, theme::pink, 1.5f);
    canvas.text(T("gui.close_title"), {r.x + 28, r.y + 24, r.w - 56, 30}, style(19, theme::text, DWRITE_FONT_WEIGHT_BOLD));
    canvas.text(text, {r.x + 28, r.y + 64, r.w - 56, textH + 4}, body);
    float by = r.bottom() - 64, bw = (r.w - 56 - 24) / 3;
    if (gui.button(id("modal-tray"), {r.x + 28, by, bw, 40}, T("gui.to_tray"), ButtonKind::Primary)) { modal_ = Modal::None; ShowWindow(hwnd, SW_HIDE); }
    if (gui.button(id("modal-quit"), {r.x + 28 + bw + 12, by, bw, 40}, T("gui.quit"), ButtonKind::Danger)) { modal_ = Modal::None; PostMessageW(hwnd, WM_APP + 10, 0, 0); }
    if (gui.button(id("modal-cancel"), {r.x + 28 + 2 * (bw + 12), by, bw, 40}, T("gui.cancel"), ButtonKind::Ghost)) modal_ = Modal::None;
}

void App::render()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = lastFrame_.QuadPart ? float(now.QuadPart - lastFrame_.QuadPart) / float(frequency_.QuadPart) : 0.016f;
    lastFrame_ = now;
    if (!canvas.begin()) return;
    gui.modal = modal_ != Modal::None;
    gui.begin(dt);
    float width = canvas.width(), height = canvas.height();
    canvas.clear(theme::background5);
    Rect area = {0, 68, width, height - 68};
    if (page_ == Page::Wizard) drawWizard(area); else drawMain(area);
    drawHeader(width); // after the page so its dropdown anchor stays on top
    gui.overlay();
    gui.drawPopup();
    if (modal_ != Modal::None) drawModal();
    gui.end();
    canvas.end();
    if (!hwnd) return;
    SetCursor(LoadCursorW(nullptr, gui.cursor));
    if (gui.wantsFrame) SetTimer(hwnd, TIMER_ANIM, 16, nullptr); else KillTimer(hwnd, TIMER_ANIM);
}

// ---------------------------------------------------------------- window

static App *g_app = nullptr;

static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_app && (g_app->hwnd == hwnd || message == WM_NCCREATE || message == WM_CREATE)) {
        if (!g_app->hwnd) g_app->hwnd = hwnd;
        return g_app->handle(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static void enableDarkMenus()
{
    // Undocumented uxtheme ordinals; absence simply keeps light menus.
    if (HMODULE theme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using SetPreferredAppMode = int(WINAPI *)(int);
        using FlushMenuThemes = void(WINAPI *)();
        auto setMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(theme, MAKEINTRESOURCEA(135)));
        auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(theme, MAKEINTRESOURCEA(136)));
        if (setMode) setMode(2);
        if (flush) flush();
    }
}

bool App::create(HINSTANCE instance, int show)
{
    QueryPerformanceFrequency(&frequency_);
    loadState();
    iconLarge_ = makeIcon(32);
    iconSmall_ = makeIcon(16);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hIcon = iconLarge_;
    wc.hIconSm = iconSmall_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    enableDarkMenus();
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    UINT dpi = GetDpiForSystem();
    RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int width = std::min(MulDiv(860, int(dpi), 96), int(work.right - work.left) - 40);
    int height = std::min(MulDiv(940, int(dpi), 96), int(work.bottom - work.top) - 40);
    hwnd = CreateWindowExW(0, kWindowClass, L"LazerASIO", WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2, width, height, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return false;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    COLORREF caption = RGB(0x1c, 0x17, 0x19), captionText = RGB(0xff, 0xff, 0xff);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption)); // DWMWA_CAPTION_COLOR
    DwmSetWindowAttribute(hwnd, 36, &captionText, sizeof(captionText)); // DWMWA_TEXT_COLOR
    if (!canvas.create(hwnd)) return false;
    canvas.setDpi(float(GetDpiForWindow(hwnd)));
    canvas.setLocale(localeFor(i18n::language));
    addTray();
    if (!deviceConfigured() || !i18n::hasSavedLanguage()) openWizard(false);
    else {
        page_ = Page::Main;
        startJob(Job::Enumerate);
        if (prefs_.autoStart) startWatching();
    }
    SetTimer(hwnd, TIMER_TICK, 100, nullptr);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    return true;
}

LRESULT App::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    const float scale = canvas.scale();
    auto toDip = [scale](LPARAM l, float &x, float &y) { x = GET_X_LPARAM(l) / scale; y = GET_Y_LPARAM(l) / scale; };
    if (message == taskbarCreated_ && taskbarCreated_) { addTray(); return 0; }
    switch (message) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); render(); EndPaint(hwnd, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: canvas.resize(LOWORD(lParam), HIWORD(lParam)); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_DPICHANGED: {
        canvas.setDpi(float(HIWORD(wParam)));
        auto *r = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(lParam);
        info->ptMinTrackSize = {LONG(640 * scale), LONG(600 * scale)};
        return 0;
    }
    case WM_MOUSEMOVE:
        toDip(lParam, gui.in.x, gui.in.y);
        if (!trackingMouse_) { TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&t); trackingMouse_ = true; }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSELEAVE: trackingMouse_ = false; if (!gui.in.down) { gui.in.x = gui.in.y = -1; } InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        toDip(lParam, gui.in.x, gui.in.y); gui.in.down = true; gui.in.pressed = true; SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONUP:
        toDip(lParam, gui.in.x, gui.in.y); gui.in.down = false; gui.in.released = true; ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_MOUSEWHEEL: {
        POINT p = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &p);
        gui.in.x = p.x / scale; gui.in.y = p.y / scale;
        gui.in.wheel += GET_WHEEL_DELTA_WPARAM(wParam) / float(WHEEL_DELTA);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CHAR: if (wParam >= 32) gui.in.chars.push_back(wchar_t(wParam)); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_KEYDOWN:
        gui.in.keys.push_back(UINT(wParam)); gui.in.ctrl = GetKeyState(VK_CONTROL) < 0;
        if (wParam == VK_ESCAPE && modal_ != Modal::None) modal_ = Modal::None;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR: if (LOWORD(lParam) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, gui.cursor)); return TRUE; } break;
    case WM_TIMER:
        if (wParam == TIMER_TICK) {
            if (volumeDirty_ && !gui.dragging() && GetTickCount64() - volumeChangedAt_ > 350) flushVolume();
            return 0;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_APP_WATCH: pullWatch(); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_APP_DEVICE: finishJob(); return 0;
    case WM_APP_TRAY:
        if (LOWORD(lParam) == WM_LBUTTONUP) { if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) ShowWindow(hwnd, SW_HIDE); else showWindow(); }
        else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) trayMenu();
        return 0;
    case WM_APP + 9: showWindow(); return 0; // second instance asked us to come forward
    case WM_APP + 10: exitApp(); return 0;    // deferred so a frame never destroys its own window
    case WM_CLOSE:
        if (watcher_.active() && prefs_.closeToTray) { modal_ = Modal::Close; showWindow(); InvalidateRect(hwnd, nullptr, FALSE); }
        else exitApp();
        return 0;
    case WM_ENDSESSION: if (wParam) { flushVolume(); watcher_.stop(); } return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// ---------------------------------------------------------------- command line

static void attachConsole()
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) return; // redirected by the caller
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
}

static int watchCommand(const std::vector<std::string> &args)
{
    std::wstring process, mutexName = L"Local\\OsuAsioStandaloneWatcher";
    int maximumWait = 0;
    bool once = false;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--process" && i + 1 < args.size()) {
            process = sanitizeProcessName(i18n::widen(args[++i]));
            if (process.empty()) { fprintf(stderr, "%s\n", i18n::text("watcher.invalid_process").c_str()); return 2; }
        }
        else if (args[i] == "--max-wait" && i + 1 < args.size()) maximumWait = atoi(args[++i].c_str());
        else if (args[i] == "--instance" && i + 1 < args.size()) mutexName += L"_" + i18n::widen(args[++i]);
        else if (args[i] == "--once") once = true;
        else { puts(i18n::text("gui.usage").c_str()); return 2; }
    }
    if (!deviceConfigured()) { fprintf(stderr, "%s\n", i18n::text("gui.first_use_cli").c_str()); return 1; }
    std::string missing;
    for (auto &name : missingBundleFiles()) missing += (missing.empty() ? "" : ", ") + name;
    if (!missing.empty()) { fprintf(stderr, "%s\n", i18n::text("gui.bundle_missing", {missing}).c_str()); return 1; }
    GameWatcher watcher;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!watcher.start(process, [wake] { SetEvent(wake); }, mutexName, maximumWait, once)) {
        fprintf(stderr, "%s\n", i18n::text("watcher.already_running").c_str());
        CloseHandle(wake);
        return 1;
    }
    for (;;) {
        WaitForSingleObject(wake, 200);
        for (auto &line : watcher.takeLog()) { puts(line.size() > 10 ? line.c_str() + 10 : line.c_str()); fflush(stdout); }
        if (watcher.finished()) break;
    }
    watcher.stop();
    for (auto &line : watcher.takeLog()) puts(line.size() > 10 ? line.c_str() + 10 : line.c_str());
    CloseHandle(wake);
    if (watcher.timedOut()) { fprintf(stderr, "%s\n", i18n::text("watcher.timeout").c_str()); return 1; }
    return watcher.result();
}

// Returns -1 when the window should open.
static int commandLine(const std::vector<std::string> &args)
{
    if (args.size() <= 1) return -1;
    attachConsole();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::vector<char *> argv;
    for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    int languageResult = i18n::languageCommand(int(argv.size()), argv.data());
    if (languageResult >= 0) return languageResult;
    const std::string &command = args[1];
    if (command == "--help" && args.size() == 2) { puts(i18n::text("gui.usage").c_str()); return 0; }
    if (command == "--show" && args.size() == 2) {
        asio::Settings settings;
        std::string error;
        const auto path = toolPath(L"config.ini");
        if (!asio::loadSettings(path, settings, error)) {
            settings = asio::Settings{};
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) fprintf(stderr, "%s\n", i18n::text("config.invalid", {error}).c_str());
        }
        fputs(formatSettings(settings).c_str(), stdout);
        return 0;
    }
    if (command == "--list" && args.size() == 2) return listDevicesCommand();
    if (command == "--watch") return watchCommand(args);
    puts(i18n::text("gui.usage").c_str());
    return 2;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    i18n::initialize();
    int count = 0;
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> args;
    for (int i = 0; i < count; ++i) args.push_back(asio::utf8Path(wide[i]));
    LocalFree(wide);
    const auto directory = asio::executableDirectory();
    if (directory.empty() || !SetCurrentDirectoryW(directory.c_str())) return 1;
    int result = commandLine(args);
    if (result >= 0) { fflush(stdout); fflush(stderr); return result; }

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\LazerASIO.Window");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) PostMessageW(existing, WM_APP + 9, 0, 0);
        CloseHandle(single);
        return 0;
    }
    auto app = std::make_unique<App>();
    g_app = app.get();
    if (!app->create(instance, show)) return 1;
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    g_app = nullptr;
    if (single) { ReleaseMutex(single); CloseHandle(single); }
    return 0;
}
