#pragma once
// Small immediate-mode toolkit drawn with Direct2D/DirectWrite. Colours follow
// osu!lazer's pink overlay scheme (OverlayColourProvider, hue 333).
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lazer::ui {

struct Color { float r, g, b, a; };
constexpr Color hex(uint32_t value, float alpha = 1.0f)
{
    return { ((value >> 16) & 0xff) / 255.0f, ((value >> 8) & 0xff) / 255.0f, (value & 0xff) / 255.0f, alpha };
}
inline Color mix(Color a, Color b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}
inline Color alpha(Color c, float a) { c.a *= a; return c; }

namespace theme {
constexpr Color background6 = hex(0x1c1719); // header, inputs
constexpr Color background5 = hex(0x2a2226); // page
constexpr Color background4 = hex(0x382e33); // cards
constexpr Color background3 = hex(0x46393f); // hover
constexpr Color background2 = hex(0x54454c); // borders
constexpr Color background1 = hex(0x705c66);
constexpr Color foreground1 = hex(0xa38f98); // secondary text
constexpr Color content2 = hex(0xf0dbe5);
constexpr Color light1 = hex(0xe0b8ca);
constexpr Color text = hex(0xffffff);
constexpr Color pink = hex(0xff66ab);         // Highlight1 / osu! pink
constexpr Color pinkDark = hex(0xbb1177);
constexpr Color green = hex(0x88b300);
constexpr Color lime = hex(0xb3d944);
constexpr Color yellow = hex(0xffcc22);
constexpr Color red = hex(0xff4d6a);
constexpr Color blue = hex(0x66ccff);
constexpr Color purple = hex(0x8866ee);
}

namespace icon {
constexpr const wchar_t *volume = L"\xE767", *mute = L"\xE74F", *refresh = L"\xE72C", *play = L"\xE768", *stop = L"\xE71A";
constexpr const wchar_t *settings = L"\xE713", *folder = L"\xE838", *document = L"\xE8A5", *chevronDown = L"\xE70D";
constexpr const wchar_t *chevronUp = L"\xE70E", *check = L"\xE73E", *back = L"\xE72B", *forward = L"\xE72A";
constexpr const wchar_t *warning = L"\xE7BA", *info = L"\xE946", *globe = L"\xE774", *headphone = L"\xE7F6";
constexpr const wchar_t *speaker = L"\xE7F5", *list = L"\xEA37", *wizard = L"\xE82D", *close = L"\xE711", *minimize = L"\xE921";
}

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(float dx, float dy) const { return { x + dx, y + dy, w - 2 * dx, h - 2 * dy }; }
    Rect inset(float d) const { return inset(d, d); }
    D2D1_RECT_F d2d() const { return D2D1::RectF(x, y, x + w, y + h); }
};

enum class Align { Left, Center, Right };
enum class Font { Ui, Icon, Mono };
struct TextStyle {
    float size = 14;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    Color color = theme::text;
    Align align = Align::Left;
    bool wrap = false;
    Font font = Font::Ui;
    bool top = false;
};
inline TextStyle style(float size, Color color, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL, Align align = Align::Left)
{
    TextStyle s; s.size = size; s.color = color; s.weight = weight; s.align = align; return s;
}

template <class T> inline void release(T *&value) { if (value) { value->Release(); value = nullptr; } }

class Canvas {
public:
    ~Canvas() { discard(); for (auto &f : formats_) f.second->Release(); release(ellipsis_); release(write_); release(factory_); }

    bool create(HWND window)
    {
        window_ = window;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(&write_)))) return false;
        uiFamily_ = pickFamily({L"Torus", L"Segoe UI Variable Text", L"Segoe UI"});
        iconFamily_ = pickFamily({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
        monoFamily_ = pickFamily({L"Cascadia Mono", L"Consolas"});
        return true;
    }

    void setLocale(const std::wstring &locale)
    {
        if (locale == locale_) return;
        locale_ = locale;
        for (auto &f : formats_) f.second->Release();
        formats_.clear();
    }

    void setDpi(float dpi) { dpi_ = dpi; if (target_) target_->SetDpi(dpi, dpi); }
    float scale() const { return dpi_ / 96.0f; }
    void resize(UINT width, UINT height) { if (target_) target_->Resize(D2D1::SizeU(width, height)); }

    bool begin()
    {
        if (!target_) {
            RECT client; GetClientRect(window_, &client);
            auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi_, dpi_);
            if (FAILED(factory_->CreateHwndRenderTarget(properties,
                    D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(UINT(client.right), UINT(client.bottom))), &target_))) return false;
            target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_);
            target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        }
        target_->BeginDraw();
        target_->SetTransform(D2D1::Matrix3x2F::Identity());
        return true;
    }
    void end() { if (target_ && target_->EndDraw() == D2DERR_RECREATE_TARGET) discard(); }

    float width() const { auto s = target_ ? target_->GetSize() : D2D1_SIZE_F{0, 0}; return s.width; }
    float height() const { auto s = target_ ? target_->GetSize() : D2D1_SIZE_F{0, 0}; return s.height; }

    void clear(Color c) { target_->Clear(D2D1::ColorF(c.r, c.g, c.b, c.a)); }
    void fill(const Rect &r, Color c, float radius = 0)
    {
        use(c);
        if (radius > 0) target_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush_);
        else target_->FillRectangle(r.d2d(), brush_);
    }
    void stroke(const Rect &r, Color c, float width = 1, float radius = 0)
    {
        use(c);
        Rect half = r.inset(width / 2);
        if (radius > 0) target_->DrawRoundedRectangle(D2D1::RoundedRect(half.d2d(), radius, radius), brush_, width);
        else target_->DrawRectangle(half.d2d(), brush_, width);
    }
    void circle(float cx, float cy, float radius, Color c)
    {
        use(c);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_);
    }
    void ring(float cx, float cy, float radius, Color c, float width)
    {
        use(c);
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_, width);
    }
    void line(float x1, float y1, float x2, float y2, Color c, float width = 1)
    {
        use(c);
        target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_, width);
    }
    void gradient(const Rect &r, Color left, Color right)
    {
        D2D1_GRADIENT_STOP stops[2] = {{0, D2D1::ColorF(left.r, left.g, left.b, left.a)}, {1, D2D1::ColorF(right.r, right.g, right.b, right.a)}};
        ID2D1GradientStopCollection *collection = nullptr;
        ID2D1LinearGradientBrush *gradient = nullptr;
        if (SUCCEEDED(target_->CreateGradientStopCollection(stops, 2, &collection)) &&
            SUCCEEDED(target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.right(), r.y)), collection, &gradient)))
            target_->FillRectangle(r.d2d(), gradient);
        release(gradient); release(collection);
    }
    void pushClip(const Rect &r) { target_->PushAxisAlignedClip(r.d2d(), D2D1_ANTIALIAS_MODE_ALIASED); }
    void popClip() { target_->PopAxisAlignedClip(); }

    void text(const std::wstring &value, const Rect &r, const TextStyle &s)
    {
        if (value.empty() || r.w <= 0) return;
        IDWriteTextLayout *layout = makeLayout(value, s, r.w, r.h);
        if (!layout) return;
        use(s.color);
        target_->DrawTextLayout(D2D1::Point2F(r.x, r.y), layout, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        layout->Release();
    }
    // Width and height of text laid out within maxWidth (no wrap when wrap=false).
    D2D1_SIZE_F measure(const std::wstring &value, const TextStyle &s, float maxWidth = 100000)
    {
        TextStyle copy = s; copy.align = Align::Left; copy.top = true;
        IDWriteTextLayout *layout = makeLayout(value.empty() ? L" " : value, copy, maxWidth, 100000);
        if (!layout) return {0, 0};
        DWRITE_TEXT_METRICS metrics = {};
        layout->GetMetrics(&metrics);
        layout->Release();
        return {value.empty() ? 0 : metrics.widthIncludingTrailingWhitespace, metrics.height};
    }
    // Caret index nearest to x for single-line text.
    size_t hitTest(const std::wstring &value, const TextStyle &s, float x)
    {
        TextStyle copy = s; copy.align = Align::Left; copy.top = true;
        IDWriteTextLayout *layout = makeLayout(value, copy, 100000, 100000);
        if (!layout) return value.size();
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics = {};
        layout->HitTestPoint(x, 1, &trailing, &inside, &metrics);
        layout->Release();
        return std::min(value.size(), size_t(metrics.textPosition + (trailing ? metrics.length : 0)));
    }

private:
    void discard() { release(brush_); release(target_); }
    void use(Color c) { brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a)); }

    std::wstring pickFamily(std::initializer_list<const wchar_t *> names)
    {
        IDWriteFontCollection *fonts = nullptr;
        std::wstring chosen = *(names.end() - 1);
        if (SUCCEEDED(write_->GetSystemFontCollection(&fonts))) {
            for (auto name : names) {
                UINT32 index = 0; BOOL exists = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists) { chosen = name; break; }
            }
            fonts->Release();
        }
        return chosen;
    }

    IDWriteTextFormat *format(const TextStyle &s)
    {
        uint64_t key = (uint64_t(s.size * 4) << 20) | (uint64_t(s.weight) << 4) | uint64_t(s.font);
        auto found = formats_.find(key);
        if (found != formats_.end()) return found->second;
        const std::wstring &family = s.font == Font::Icon ? iconFamily_ : s.font == Font::Mono ? monoFamily_ : uiFamily_;
        IDWriteTextFormat *created = nullptr;
        if (FAILED(write_->CreateTextFormat(family.c_str(), nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                s.size, locale_.c_str(), &created))) return nullptr;
        formats_[key] = created;
        return created;
    }

    IDWriteTextLayout *makeLayout(const std::wstring &value, const TextStyle &s, float w, float h)
    {
        IDWriteTextFormat *f = format(s);
        if (!f) return nullptr;
        IDWriteTextLayout *layout = nullptr;
        if (FAILED(write_->CreateTextLayout(value.c_str(), UINT32(value.size()), f, std::max(w, 1.0f), std::max(h, 1.0f), &layout))) return nullptr;
        layout->SetTextAlignment(s.align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : s.align == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetParagraphAlignment(s.top ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        layout->SetWordWrapping(s.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!s.wrap) {
            if (!ellipsis_) write_->CreateEllipsisTrimmingSign(f, &ellipsis_);
            DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trimming, ellipsis_);
        }
        return layout;
    }

    HWND window_ = nullptr;
    ID2D1Factory *factory_ = nullptr;
    ID2D1HwndRenderTarget *target_ = nullptr;
    ID2D1SolidColorBrush *brush_ = nullptr;
    IDWriteFactory *write_ = nullptr;
    IDWriteInlineObject *ellipsis_ = nullptr;
    std::unordered_map<uint64_t, IDWriteTextFormat *> formats_;
    std::wstring uiFamily_, iconFamily_, monoFamily_, locale_ = L"en-us";
    float dpi_ = 96;
};

struct Input {
    float x = -1, y = -1;
    bool down = false, pressed = false, released = false;
    float wheel = 0;
    std::wstring chars;
    std::vector<UINT> keys;
    bool ctrl = false;
};

inline uint64_t id(const char *name, uint64_t index = 0)
{
    uint64_t hash = 1469598103934665603ull;
    for (const char *p = name; *p; ++p) { hash ^= uint8_t(*p); hash *= 1099511628211ull; }
    return hash ^ (index * 0x9E3779B97F4A7C15ull);
}

enum class ButtonKind { Primary, Secondary, Ghost, Danger };

class Gui {
public:
    Canvas &c;
    Input in;
    float dt = 0.016f;
    LPCWSTR cursor = IDC_ARROW;
    bool modal = false;          // set by the app before drawing the base layer
    bool wantsFrame = false;     // an animation or deferred result needs another frame

    explicit Gui(Canvas &canvas) : c(canvas) {}

    void begin(float delta)
    {
        dt = std::clamp(delta, 0.001f, 0.1f);
        wantsFrame = false;
        cursor = IDC_ARROW;
        base_ = true;
        overPopup_ = popup_ && popupRect_.contains(in.x, in.y);
        if (popup_ && in.pressed && !overPopup_ && !popupAnchor_.contains(in.x, in.y)) { popup_ = 0; in.pressed = false; consumed_ = true; }
        if (in.pressed && focus_ && !focusRect_.contains(in.x, in.y)) focus_ = 0;
        caretTime_ += dt;
    }
    void overlay() { base_ = false; }
    void end()
    {
        if (!in.down) active_ = 0;
        if (popup_ && popupDrawn_ != popup_) popup_ = 0;
        popupDrawn_ = 0;
        in.pressed = in.released = false;
        in.wheel = 0;
        in.chars.clear();
        in.keys.clear();
        consumed_ = false;
    }

    bool enabledInput() const { return !(base_ && (modal || overPopup_)); }
    bool hovering(const Rect &r) const { return enabledInput() && r.contains(in.x, in.y); }
    bool popupOpen() const { return popup_ != 0; }
    bool overPopup() const { return overPopup_; }
    bool focused() const { return focus_ != 0; }
    bool dragging() const { return active_ != 0; }

    float anim(uint64_t key, float target, float speed = 14)
    {
        auto found = anims_.find(key);
        float &value = found == anims_.end() ? (anims_[key] = target) : found->second;
        value += (target - value) * (1 - std::exp(-speed * dt));
        if (std::fabs(target - value) > 0.002f) wantsFrame = true; else value = target;
        return value;
    }

    bool button(uint64_t key, const Rect &r, const std::wstring &label, ButtonKind kind = ButtonKind::Primary, bool enabled = true, const wchar_t *glyph = nullptr)
    {
        bool hot = enabled && hovering(r);
        if (hot) cursor = IDC_HAND;
        if (hot && in.pressed) active_ = key;
        bool clicked = hot && in.released && active_ == key;
        float h = anim(key, hot ? (in.down && active_ == key ? 2.0f : 1.0f) : 0.0f, 18);
        Color fill, text = theme::text, border = {0, 0, 0, 0};
        switch (kind) {
        case ButtonKind::Primary: fill = mix(theme::pink, hex(0xff8cc0), std::min(h, 1.0f)); if (h > 1) fill = mix(fill, theme::pinkDark, (h - 1) * 0.5f); text = hex(0x1c1719); break;
        case ButtonKind::Secondary: fill = mix(theme::background3, theme::background2, std::min(h, 1.0f)); break;
        case ButtonKind::Ghost: fill = alpha(theme::background3, std::min(h, 1.0f)); border = theme::background2; text = theme::content2; break;
        case ButtonKind::Danger: fill = mix(hex(0x5c2a35), hex(0x7a3443), std::min(h, 1.0f)); text = hex(0xffc2cc); break;
        }
        if (!enabled) { fill = alpha(fill, 0.35f); text = alpha(text, 0.4f); }
        c.fill(r, fill, 6);
        if (border.a > 0) c.stroke(r, alpha(border, enabled ? 1 : 0.5f), 1, 6);
        TextStyle s = style(14, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
        if (glyph && !label.empty()) {
            auto size = c.measure(label, s);
            float total = 16 + 8 + size.width, x = r.x + (r.w - total) / 2;
            TextStyle i = s; i.font = Font::Icon; i.size = 14; i.align = Align::Left; i.weight = DWRITE_FONT_WEIGHT_NORMAL;
            c.text(glyph, {x, r.y, 18, r.h}, i);
            s.align = Align::Left;
            c.text(label, {x + 24, r.y, size.width + 4, r.h}, s);
        } else if (glyph) {
            TextStyle i = s; i.font = Font::Icon; i.size = 16; i.weight = DWRITE_FONT_WEIGHT_NORMAL;
            c.text(glyph, r, i);
        } else c.text(label, r.inset(8, 0), s);
        return clicked && enabled;
    }

    bool iconButton(uint64_t key, const Rect &r, const wchar_t *glyph, bool enabled = true, Color tint = theme::content2)
    {
        bool hot = enabled && hovering(r);
        if (hot) cursor = IDC_HAND;
        if (hot && in.pressed) active_ = key;
        bool clicked = hot && in.released && active_ == key;
        float h = anim(key, hot ? 1.0f : 0.0f, 18);
        c.fill(r, alpha(theme::background3, h), r.h / 2);
        TextStyle i = style(16, enabled ? mix(tint, theme::text, h) : alpha(tint, 0.35f), DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
        i.font = Font::Icon;
        c.text(glyph, r, i);
        return clicked && enabled;
    }

    // osu!-style slider: thin track, pink fill and a pill nub that fills when held.
    bool slider(uint64_t key, const Rect &r, int &value, int minimum, int maximum, bool enabled = true)
    {
        const float nubW = 28, nubH = 14;
        Rect track = {r.x + nubW / 2, r.y + r.h / 2 - 2, r.w - nubW, 4};
        bool hot = enabled && hovering(r);
        if (hot) cursor = IDC_HAND;
        if (hot && in.pressed) active_ = key;
        int before = value;
        if (active_ == key && (in.down || in.pressed) && enabled) {
            float t = std::clamp((in.x - track.x) / track.w, 0.0f, 1.0f);
            value = minimum + int(std::lround(t * (maximum - minimum)));
        }
        if (hot && in.wheel != 0 && enabled) value = std::clamp(value + (in.wheel > 0 ? 1 : -1), minimum, maximum);
        float t = maximum > minimum ? float(value - minimum) / float(maximum - minimum) : 0;
        float shown = anim(key ^ 0x51, t, 30);
        float h = anim(key, active_ == key ? 1.0f : hot ? 0.6f : 0.0f, 16);
        c.fill(track, enabled ? theme::background2 : alpha(theme::background2, 0.5f), 2);
        Rect filled = track; filled.w = track.w * shown;
        Color accent = enabled ? theme::pink : theme::background1;
        c.fill(filled, accent, 2);
        Rect nub = {track.x + track.w * shown - nubW / 2, r.y + r.h / 2 - nubH / 2, nubW, nubH};
        c.fill(nub, mix(theme::background5, accent, h), nubH / 2);
        c.stroke(nub, accent, 2, nubH / 2);
        return value != before;
    }
    bool sliderActive(uint64_t key) const { return active_ == key; }

    bool toggle(uint64_t key, const Rect &r, bool &value, bool enabled = true)
    {
        Rect pill = {r.x, r.y + (r.h - 20) / 2, 40, 20};
        bool hot = enabled && hovering(r);
        if (hot) cursor = IDC_HAND;
        if (hot && in.pressed) active_ = key;
        bool clicked = hot && in.released && active_ == key;
        if (clicked) value = !value;
        float t = anim(key, value ? 1.0f : 0.0f, 16);
        float h = anim(key ^ 0x77, hot ? 1.0f : 0.0f, 16);
        Color accent = enabled ? theme::pink : theme::background1;
        c.fill(pill, mix(mix(theme::background6, theme::background5, h), alpha(accent, 0.85f), t), 10);
        c.stroke(pill, mix(theme::background2, accent, t), 2, 10);
        c.circle(pill.x + 10 + 20 * t, pill.y + 10, 6, mix(theme::foreground1, theme::text, std::max(t, h)));
        return clicked;
    }

    bool segmented(uint64_t key, const Rect &r, const std::vector<std::wstring> &options, int &selected, bool enabled = true)
    {
        int before = selected;
        c.fill(r, theme::background6, 6);
        float w = r.w / float(options.size());
        float shown = anim(key, float(selected), 18);
        c.fill({r.x + 3 + w * shown, r.y + 3, w - 6, r.h - 6}, enabled ? theme::pink : theme::background2, 4);
        for (size_t i = 0; i < options.size(); ++i) {
            Rect cell = {r.x + w * i, r.y, w, r.h};
            bool hot = enabled && hovering(cell);
            if (hot) cursor = IDC_HAND;
            if (hot && in.pressed) active_ = key + i + 1;
            if (hot && in.released && active_ == key + i + 1) selected = int(i);
            bool on = int(i) == selected;
            Color text = on ? hex(0x1c1719) : hot ? theme::text : theme::foreground1;
            c.text(options[i], cell.inset(6, 0), style(13.5f, enabled ? text : alpha(text, 0.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center));
        }
        return selected != before;
    }

    // Dropdown with a deferred popup: a pick made in the overlay is returned
    // by the same widget on the next frame.
    bool dropdown(uint64_t key, const Rect &r, const std::vector<std::wstring> &items, int &selected, bool enabled = true, const std::wstring &placeholder = L"")
    {
        bool changed = false;
        if (chosenFor_ == key) { if (chosen_ >= 0 && chosen_ < int(items.size()) && chosen_ != selected) { selected = chosen_; changed = true; } chosenFor_ = 0; }
        bool open = popup_ == key;
        bool hot = enabled && (hovering(r) || (open && r.contains(in.x, in.y)));
        if (hot) cursor = IDC_HAND;
        if (hot && in.pressed && !consumed_) { popup_ = open ? 0 : key; open = !open; popupScroll_ = 0; if (open) { popupAnchor_ = r; popupItems_ = items; popupSelected_ = selected; } }
        float h = anim(key, hot || open ? 1.0f : 0.0f, 18);
        c.fill(r, mix(theme::background6, theme::background5, h), 6);
        c.stroke(r, open ? theme::pink : mix(theme::background2, theme::background1, h), open ? 1.5f : 1, 6);
        std::wstring label = selected >= 0 && selected < int(items.size()) ? items[size_t(selected)] : placeholder;
        c.text(label, {r.x + 12, r.y, r.w - 44, r.h}, style(14, enabled ? theme::text : theme::foreground1));
        TextStyle i = style(12, enabled ? theme::light1 : theme::background1, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
        i.font = Font::Icon;
        c.text(open ? icon::chevronUp : icon::chevronDown, {r.right() - 34, r.y, 26, r.h}, i);
        if (open) { popupItems_ = items; popupAnchor_ = r; popupDrawn_ = key; }
        return changed;
    }

    // Draw the open dropdown list; call after the base layer.
    void drawPopup()
    {
        if (!popup_ || popupDrawn_ != popup_) return;
        const float row = 34, maxRows = 8;
        float visible = std::min(float(popupItems_.size()), maxRows);
        Rect list = {popupAnchor_.x, popupAnchor_.bottom() + 4, popupAnchor_.w, visible * row + 8};
        if (list.bottom() > c.height() - 8) list.y = popupAnchor_.y - 4 - list.h;
        popupRect_ = list;
        float maxScroll = std::max(0.0f, (float(popupItems_.size()) - maxRows) * row);
        if (list.contains(in.x, in.y) && in.wheel != 0) { popupScroll_ = std::clamp(popupScroll_ - in.wheel * row, 0.0f, maxScroll); in.wheel = 0; }
        float open = anim(popup_ ^ 0x9090, 1.0f, 22);
        c.fill({list.x, list.y + 3, list.w, list.h}, hex(0x000000, 0.35f * open), 8);
        c.fill(list, alpha(theme::background6, open), 8);
        c.stroke(list, alpha(theme::background2, open), 1, 8);
        c.pushClip(list.inset(4));
        for (size_t i = 0; i < popupItems_.size(); ++i) {
            Rect item = {list.x + 4, list.y + 4 + row * i - popupScroll_, list.w - 8, row};
            if (item.bottom() < list.y || item.y > list.bottom()) continue;
            bool hot = item.contains(in.x, in.y) && list.inset(4).contains(in.x, in.y);
            if (hot) cursor = IDC_HAND;
            bool on = int(i) == popupSelected_;
            if (hot) c.fill(item, theme::background4, 5);
            if (on) c.fill({item.x + 4, item.y + 9, 3, row - 18}, theme::pink, 1.5f);
            c.text(popupItems_[i], {item.x + 14, item.y, item.w - 20, item.h}, style(14, on ? theme::pink : hot ? theme::text : theme::content2,
                on ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL));
            if (hot && in.released) { chosen_ = int(i); chosenFor_ = popup_; popup_ = 0; wantsFrame = true; }
        }
        c.popClip();
        if (popup_) popupDrawn_ = popup_;
    }

    bool textInput(uint64_t key, const Rect &r, std::wstring &value, size_t limit, bool enabled = true)
    {
        bool hot = enabled && hovering(r);
        if (hot) cursor = IDC_IBEAM;
        TextStyle s = style(14, enabled ? theme::text : theme::foreground1);
        Rect inner = {r.x + 12, r.y, r.w - 24, r.h};
        if (hot && in.pressed) { focus_ = key; caret_ = c.hitTest(value, s, in.x - inner.x); caretTime_ = 0; }
        bool changed = false;
        bool focusedNow = focus_ == key && enabled;
        if (focusedNow) {
            focusRect_ = r;
            caret_ = std::min(caret_, value.size());
            for (wchar_t ch : in.chars) {
                if (ch >= 32 && ch != 127 && value.size() < limit) { value.insert(caret_++, 1, ch); changed = true; }
            }
            for (UINT k : in.keys) {
                if (k == VK_BACK && caret_ > 0) { value.erase(--caret_, 1); changed = true; }
                else if (k == VK_DELETE && caret_ < value.size()) { value.erase(caret_, 1); changed = true; }
                else if (k == VK_LEFT && caret_ > 0) --caret_;
                else if (k == VK_RIGHT && caret_ < value.size()) ++caret_;
                else if (k == VK_HOME) caret_ = 0;
                else if (k == VK_END) caret_ = value.size();
                else if (k == VK_RETURN || k == VK_ESCAPE) focus_ = 0;
                else if (k == 'V' && in.ctrl) changed |= paste(value, limit);
                caretTime_ = 0;
            }
        }
        float h = anim(key, focusedNow ? 1.0f : hot ? 0.5f : 0.0f, 18);
        c.fill(r, mix(theme::background6, theme::background5, h * 0.5f), 6);
        c.stroke(r, mix(theme::background2, theme::pink, h), focusedNow ? 1.5f : 1, 6);
        c.pushClip(inner);
        c.text(value, {inner.x, inner.y, 100000, inner.h}, s);
        if (focusedNow) {
            if (std::fmod(caretTime_, 1.0f) < 0.55f) {
                float x = inner.x + c.measure(value.substr(0, caret_), s).width;
                c.line(x + 0.5f, r.y + 9, x + 0.5f, r.bottom() - 9, theme::pink, 1.5f);
            }
            wantsFrame = true;
        }
        c.popClip();
        return changed;
    }

    // Selectable row (device list).
    bool selectable(uint64_t key, const Rect &r, bool selected, bool enabled = true)
    {
        bool hot = enabled && hovering(r);
        if (hot) cursor = IDC_HAND;
        if (hot && in.pressed) active_ = key;
        bool clicked = hot && in.released && active_ == key;
        float h = anim(key, hot ? 1.0f : 0.0f, 18);
        float s = anim(key ^ 0x3131, selected ? 1.0f : 0.0f, 18);
        c.fill(r, mix(mix(theme::background5, theme::background3, h), mix(theme::background3, hex(0x5a3348), 0.6f), s), 8);
        if (s > 0.01f) c.stroke(r, alpha(theme::pink, s), 1.5f, 8);
        return clicked && enabled;
    }

    bool clicked(uint64_t key, const Rect &r)
    {
        bool hot = hovering(r);
        if (hot) cursor = IDC_HAND;
        if (hot && in.pressed) active_ = key;
        return hot && in.released && active_ == key;
    }

private:
    bool paste(std::wstring &value, size_t limit)
    {
        bool changed = false;
        if (!OpenClipboard(nullptr)) return false;
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (auto text = static_cast<const wchar_t *>(GlobalLock(data))) {
                for (const wchar_t *p = text; *p && value.size() < limit; ++p)
                    if (*p >= 32 && *p != 127) { value.insert(caret_++, 1, *p); changed = true; }
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        return changed;
    }

    bool base_ = true, overPopup_ = false, consumed_ = false;
    uint64_t active_ = 0, focus_ = 0, popup_ = 0, popupDrawn_ = 0, chosenFor_ = 0;
    int chosen_ = -1, popupSelected_ = -1;
    Rect popupRect_, popupAnchor_, focusRect_;
    std::vector<std::wstring> popupItems_;
    float popupScroll_ = 0, caretTime_ = 0;
    size_t caret_ = 0;
    std::unordered_map<uint64_t, float> anims_;
};

} // namespace lazer::ui
