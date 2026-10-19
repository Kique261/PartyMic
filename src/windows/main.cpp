#include "windows/audio_engine.h"
#include "windows/devices.h"
#include "windows/preview.h"
#include "windows/page_transition.h"
#include "windows/ui_theme.h"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <cwctype>
#include <exception>
#include <shellapi.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace partymic;
enum : int {
    Refresh = 101,
    Music,
    Microphone,
    Cable,
    MusicVolume,
    MicVolume,
    MuteMusic,
    MuteMic,
    Ducking,
    Start,
    Stop,
    DuckFloor,
    RecordSource,
    Record,
    StopPreview,
    PlayPreview,
    PreviewOutput,
    PreviewVolume,
    OpenRecordings,
    NavMix = 201,
    NavRecord,
    NavGuide,
    RecordShortcut
};
constexpr int design_width = 1120;
enum Anchor { Fixed, Stretch, Right, HalfWidth, SecondColumn, SecondRight };
struct Control {
    HWND window;
    int x, y, w, h, mask;
    Anchor anchor;
    int weight;
};
class App {
  public:
    HWND window = nullptr;
    AudioEngine engine;
    AudioPreview preview;
    std::vector<Endpoint> microphones, cables, speakers;
    std::vector<Application> applications;
    std::vector<Control> controls;
    HWND music_combo{}, mic_combo{}, cable_combo{}, music_slider{}, mic_slider{};
    HWND music_value{}, mic_value{}, music_meter{}, mic_meter{}, output_meter{};
    HWND mute_music{}, mute_mic{}, duck{}, start{}, stop{}, refresh_button{}, status{}, detail{};
    HWND duck_slider{}, duck_value{}, record_source{}, record_button{}, preview_stop{}, play_button{};
    HWND preview_output{}, preview_slider{}, preview_value{}, preview_status{}, preview_meter{},
        open_recordings{};
    HWND nav_mix{}, nav_record{}, nav_guide{};
    HFONT font = nullptr, title_font = nullptr, small_font = nullptr, heading_font = nullptr,
          value_font = nullptr;
    HBRUSH background = CreateSolidBrush(ui::white);
    UINT dpi = 96;
    int page = 0, scroll_offset = 0, wheel_remainder = 0;
    bool previous_active = false;
    HICON small_icon = nullptr, large_icon = nullptr, brand_icon = nullptr;
    ui::PageTransition page_transition;
    ULONGLONG badge_started = 0;
    bool page_moving = false, badge_moving = false;
    void update_icons() {
        auto load = [&](int w, int h) {
            return static_cast<HICON>(
                LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, w, h, 0));
        };
        auto loaded_small = load(GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi));
        auto loaded_large = load(GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi));
        auto loaded_brand = load(px(32), px(32));
        if (!loaded_small || !loaded_large || !loaded_brand) {
            for (auto h : {loaded_small, loaded_large, loaded_brand})
                if (h)
                    DestroyIcon(h);
            throw std::runtime_error("Cannot load PartyMic icon resource");
        }
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(loaded_small));
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(loaded_large));
        for (auto h : {small_icon, large_icon, brand_icon})
            if (h)
                DestroyIcon(h);
        small_icon = loaded_small;
        large_icon = loaded_large;
        brand_icon = loaded_brand;
    }
    void stop_motion() {
        page_moving = badge_moving = false;
        page_transition.stop();
        KillTimer(window, 2);
        for (const auto &c : controls)
            ui::settle(c.window);
    }
    void begin_badge_motion() {
        if (!ui::motion_enabled() || IsIconic(window))
            return;
        badge_started = GetTickCount64();
        badge_moving = true;
        if (!SetTimer(window, 2, 10, nullptr))
            badge_moving = false;
    }
    void motion_tick() {
        if (!ui::motion_enabled() || IsIconic(window)) {
            stop_motion();
            position_controls();
            return;
        }
        const auto now = GetTickCount64();
        if (page_moving) {
            page_transition.tick();
            page_moving = page_transition.active();
        }
        if (badge_moving) {
            badge_moving = now - badge_started < 360;
            RECT r{px(logical_width() - 170), px(28), px(logical_width() - 22), px(74)};
            InvalidateRect(window, &r, FALSE);
        }
        if (!page_moving && !badge_moving)
            KillTimer(window, 2);
    }
    ~App() {
        page_transition.stop();
        preview.stop();
        engine.stop();
        for (auto f : {font, title_font, small_font, heading_font, value_font})
            if (f)
                DeleteObject(f);
        DeleteObject(background);
        for (auto h : {small_icon, large_icon, brand_icon})
            if (h)
                DestroyIcon(h);
    }
    int px(int n) const { return MulDiv(n, int(dpi), 96); }
    int logical_width() const {
        RECT r{};
        GetClientRect(window, &r);
        return MulDiv(r.right, 96, dpi);
    }
    int content_height() const { return page == 2 ? 760 : 820; }
    HWND add(const wchar_t *type, const wchar_t *label, DWORD style, int x, int y, int w, int h, int id = 0,
             int mask = 1, Anchor anchor = Fixed, int weight = 0) {
        HWND child = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, px(x),
                                     px(y), px(w), px(h), window, reinterpret_cast<HMENU>(INT_PTR(id)),
                                     GetModuleHandleW(nullptr), nullptr);
        if (!child)
            throw std::runtime_error("Cannot create UI control");
        controls.push_back({child, x, y, w, h, mask, anchor, weight});
        return child;
    }
    HWND caption(const wchar_t *label, int x, int y, int w, int h = 24, int mask = 1,
                 COLORREF color = ui::muted, Anchor anchor = Fixed, int weight = 0) {
        auto hnd = add(L"STATIC", label, SS_LEFT, x, y, w, h, 0, mask, anchor, weight);
        ui::text_color(hnd, color);
        return hnd;
    }
    HWND button(const wchar_t *label, int x, int y, int w, int h, int id, int mask, ui::Kind kind,
                ui::Icon icon = ui::Icon::None, Anchor anchor = Fixed) {
        auto hnd = add(L"BUTTON", label, WS_TABSTOP | BS_PUSHBUTTON, x, y, w, h, id, mask, anchor);
        ui::style(hnd, kind, icon);
        return hnd;
    }
    HWND combo(int x, int y, int w, int id, int mask = 1, Anchor anchor = Stretch) {
        auto hnd = add(WC_COMBOBOXW, L"",
                       CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, x, y,
                       w, 220, id, mask, anchor);
        ui::style(hnd, ui::Kind::Combo);
        return hnd;
    }
    HWND slider(int x, int y, int w, int id, int minimum, int maximum, int initial, int mask, Anchor anchor,
                COLORREF accent = ui::teal) {
        auto hnd =
            add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x, y, w, 32, id, mask, anchor);
        SendMessageW(hnd, TBM_SETRANGE, TRUE, MAKELPARAM(minimum, maximum));
        SendMessageW(hnd, TBM_SETPOS, TRUE, initial);
        SendMessageW(hnd, TBM_SETPAGESIZE, 0, 10);
        ui::style(hnd, ui::Kind::Slider, ui::Icon::None, accent);
        return hnd;
    }
    HWND check(const wchar_t *label, int x, int y, int w, int id, int mask = 1, Anchor anchor = Fixed) {
        auto hnd = add(L"BUTTON", label, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 28, id, mask, anchor);
        ui::style(hnd, ui::Kind::Check);
        return hnd;
    }
    HWND meter(int x, int y, int w, int mask, COLORREF accent = ui::teal, Anchor anchor = Fixed) {
        auto hnd = add(PROGRESS_CLASSW, L"", PBS_SMOOTH, x, y, w, 16, 0, mask, anchor);
        ui::style(hnd, ui::Kind::Meter, ui::Icon::None, accent);
        return hnd;
    }
    void position_controls(bool synchronous = true) {
        RECT r{};
        GetClientRect(window, &r);
        const int maximum = std::max(0, px(content_height()) - int(r.bottom));
        scroll_offset = std::clamp(scroll_offset, 0, maximum);
        SCROLLINFO info{sizeof(info),
                        SIF_RANGE | SIF_PAGE | SIF_POS,
                        0,
                        px(content_height()) - 1,
                        UINT(std::max(0L, r.bottom)),
                        scroll_offset,
                        0};
        SetScrollInfo(window, SB_VERT, &info, TRUE);
        const int delta = logical_width() - design_width;
        HDWP batch = BeginDeferWindowPos(int(controls.size()));
        for (const auto &c : controls) {
            const bool visible = (c.mask & (1 << page)) != 0;
            if (!visible) {
                if (IsWindowVisible(c.window)) {
                    const UINT flags = SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                       SWP_NOACTIVATE | SWP_NOREDRAW;
                    if (batch)
                        batch = DeferWindowPos(batch, c.window, nullptr, 0, 0, 0, 0, flags);
                    else
                        SetWindowPos(c.window, nullptr, 0, 0, 0, 0, flags);
                }
                continue;
            }
            int x = c.x, y = c.y, w = c.w, h = c.h;
            if (c.anchor == Stretch)
                w += delta;
            else if (c.anchor == Right)
                x += delta;
            else if (c.anchor == HalfWidth)
                w += delta / 2;
            else if (c.anchor == SecondColumn) {
                x += delta / 2;
                w += delta / 2;
            } else if (c.anchor == SecondRight)
                x += delta / 2;
            const int id = GetDlgCtrlID(c.window);
            if (page == 1) {
                if (c.window == mic_slider) {
                    x = 276;
                    y = 430;
                    w = 610 + delta;
                } else if (c.window == mic_value) {
                    x = 974 + delta;
                    y = 380;
                    w = 90;
                } else if (c.window == mute_mic) {
                    x = 920 + delta;
                    y = 430;
                    w = 144;
                }
            }
            if (c.mask != 7)
                y -= MulDiv(scroll_offset, 96, dpi);
            // Queue visibility with geometry; never briefly show a scrolled-off control.
            const bool shown = c.mask == 7 || px(y) >= px(110);
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW;
            if (bool(IsWindowVisible(c.window)) != shown)
                flags |= shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
            if (batch)
                batch =
                    DeferWindowPos(batch, c.window, nullptr, px(x), px(y), px(std::max(20, w)), px(h), flags);
            else
                SetWindowPos(c.window, nullptr, px(x), px(y), px(std::max(20, w)), px(h), flags);

            if (id == Music || id == Microphone || id == Cable || id == RecordSource || id == PreviewOutput) {
                if (SendMessageW(c.window, CB_GETITEMHEIGHT, WPARAM(-1), 0) != px(36))
                    SendMessageW(c.window, CB_SETITEMHEIGHT, WPARAM(-1), px(36));
                if (SendMessageW(c.window, CB_GETITEMHEIGHT, 0, 0) != px(34))
                    SendMessageW(c.window, CB_SETITEMHEIGHT, 0, px(34));
            }
        }
        if (batch)
            EndDeferWindowPos(batch);
        // A staged destination is captured explicitly under the content shield.
        // Queue its live repaint instead of synchronously rendering it twice.
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN | (synchronous ? RDW_UPDATENOW : 0));
    }
    void scroll(int cmd, int wheel = 0) {
        page_moving = false;
        page_transition.stop();
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_ALL;
        GetScrollInfo(window, SB_VERT, &info);
        switch (cmd) {
        case SB_TOP:
            scroll_offset = 0;
            break;
        case SB_BOTTOM:
            scroll_offset = px(content_height());
            break;
        case SB_LINEUP:
            scroll_offset -= px(32);
            break;
        case SB_LINEDOWN:
            scroll_offset += px(32);
            break;
        case SB_PAGEUP:
            scroll_offset -= int(info.nPage);
            break;
        case SB_PAGEDOWN:
            scroll_offset += int(info.nPage);
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            scroll_offset = info.nTrackPos;
            break;
        default:
            scroll_offset -= wheel * px(44);
            break;
        }
        position_controls();
    }
    void navigate(int next) {
        if (page == next)
            return;
        RECT content{};
        GetClientRect(window, &content);
        content.left = px(221);
        content.top = px(110);
        const bool animate =
            ui::motion_enabled() && !IsIconic(window) && page_transition.prepare(window, content);
        page = next;
        scroll_offset = 0;
        ui::selected(nav_mix, page == 0);
        ui::selected(nav_record, page == 1);
        ui::selected(nav_guide, page == 2);
        position_controls(!animate);
        SetFocus(page == 0 ? music_combo : (page == 1 ? record_source : nav_guide));
        page_moving = animate && page_transition.start();
        if (page_moving && !SetTimer(window, 2, 10, nullptr)) {
            page_moving = false;
            page_transition.stop();
        }
    }
    void layout() {
        for (auto f : {font, title_font, small_font, heading_font, value_font})
            if (f)
                DeleteObject(f);
        auto make = [&](int size, int weight) {
            return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                               L"Microsoft YaHei UI");
        };
        font = make(14, FW_NORMAL);
        title_font = make(23, FW_SEMIBOLD);
        small_font = make(12, FW_NORMAL);
        heading_font = make(17, FW_SEMIBOLD);
        value_font = make(25, FW_SEMIBOLD);
        for (const auto &c : controls)
            SendMessageW(c.window, WM_SETFONT,
                         reinterpret_cast<WPARAM>(
                             c.weight == 2
                                 ? value_font
                                 : (c.weight == 1 ? heading_font : (c.weight == -1 ? small_font : font))),
                         FALSE);
        position_controls();
    }
    void paint(HDC target) {
        RECT bounds{};
        GetClientRect(window, &bounds);
        HDC dc = CreateCompatibleDC(target);
        auto bitmap = CreateCompatibleBitmap(target, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
        auto old = SelectObject(dc, bitmap);
        FillRect(dc, &bounds, background);
        RECT side{0, 0, px(220), bounds.bottom};
        auto brush = CreateSolidBrush(ui::sidebar);
        FillRect(dc, &side, brush);
        DeleteObject(brush);
        ui::line(dc, px(220), 0, px(220), bounds.bottom, ui::border);
        auto rect = [&](int x, int y, int w, int h, bool scrolling = true) {
            if (scrolling)
                y -= MulDiv(scroll_offset, 96, dpi);
            return RECT{px(x), px(y), px(x + w), px(y + h)};
        };
        auto txt = [&](const wchar_t *s, int x, int y, int w, int h, HFONT f, COLORREF color,
                       bool scrolling = true) { ui::text(dc, s, rect(x, y, w, h, scrolling), f, color); };
        DrawIconEx(dc, px(24), px(28), brand_icon, px(32), px(32), 0, nullptr, DI_NORMAL);
        txt(L"PartyMic", 68, 28, 140, 32, heading_font, ui::ink, false);
        txt(L"把音乐带进每一次开黑", 25, 72, 185, 24, small_font, ui::muted, false);
        txt(L"工作空间", 26, 121, 160, 20, small_font, ui::muted, false);
        const int base = MulDiv(bounds.bottom, 96, dpi) - 116;
        ui::line(dc, px(24), px(base), px(196), px(base), ui::border);
        ui::icon(dc, ui::Icon::Headphones, rect(26, base + 23, 18, 18, false), ui::teal);
        txt(L"本地音频 · 随时可控", 54, base + 19, 150, 26, small_font, ui::muted, false);
        txt(L"PartyMic  0.5.1", 26, base + 67, 180, 24, small_font, ui::muted, false);
        const int delta = logical_width() - design_width;
        const wchar_t *titles[] = {L"混音工作台", L"录音与试听", L"使用指南"};
        const wchar_t *subtitles[] = {L"喜欢的音乐，和你的声音一起出发。",
                                      L"先听见自己的声音，再找到合适的平衡。",
                                      L"简单设置，让每次开黑都有好声音。"};
        txt(titles[page], 252, 23, 450, 36, title_font, ui::ink, false);
        txt(subtitles[page], 252, 65, 650, 24, font, ui::muted, false);
        ui::line(dc, px(220), px(108), bounds.right, px(108), ui::border);
        RECT badge = rect(958 + delta, 35, 130, 30, false);
        ui::rounded(dc, badge, px(15), ui::teal_soft);
        if (badge_moving) {
            const float t = std::clamp(float(GetTickCount64() - badge_started) / 360, 0.0f, 1.0f);
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen pen(Gdiplus::Color(BYTE(100 * (1 - t)), 13, 148, 136), float(px(1)));
            const float radius = px(4) + t * px(6), cx = badge.left + px(25),
                        cy = (badge.top + badge.bottom) / 2.0f;
            g.DrawEllipse(&pen, cx - radius, cy - radius, radius * 2, radius * 2);
        }
        ui::text(dc,
                 engine.active()
                     ? L"●  正在混音"
                     : (preview.recording() ? L"●  正在录音"
                                            : (preview.playing() ? L"●  正在试听" : L"●  准备就绪")),
                 badge, small_font, ui::teal_text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const int content_clip = SaveDC(dc);
        IntersectClipRect(dc, px(221), px(110), bounds.right, bounds.bottom);
        auto card = [&](int x, int y, int w, int h, COLORREF stroke = ui::border) {
            ui::rounded(dc, rect(x, y + 1, w, h), px(13), RGB(248, 248, 250));
            ui::rounded(dc, rect(x, y, w, h), px(13), ui::white, stroke);
        };
        RECT content_area{px(221), px(110), bounds.right, bounds.bottom};
        // During a fade the opaque child shield clips this entire area out of
        // WM_PAINT. Header/sidebar updates must not rasterize hidden cards.
        if (RectVisible(target, &content_area)) {
            if (page == 0) {
                txt(L"音源与设备", 252, 122, 300, 24, heading_font, ui::ink);
                card(252, 166, 836 + delta, 222, engine.active() ? ui::teal : ui::border);
                txt(L"音频来源", 276, 180, 250, 28, heading_font, ui::ink);
                txt(L"只采集你选择的音乐软件", 786 + delta, 184, 278, 24, small_font, ui::muted);
                for (int i = 0; i < 3; ++i) {
                    const ui::Icon shapes[] = {ui::Icon::Music, ui::Icon::Mic, ui::Icon::Cable};
                    ui::icon(dc, shapes[i], rect(276, 229 + i * 50, 20, 20), i == 0 ? ui::orange : ui::muted);
                }
                const int half = 408 + delta / 2;
                card(252, 408, half, 166);
                card(680 + delta / 2, 408, half, 166);
                ui::rounded(dc, rect(276, 430, 34, 34), px(9), ui::orange_soft);
                ui::icon(dc, ui::Icon::Music, rect(284, 438, 18, 18), ui::orange);
                txt(L"音乐音量", 322, 434, 160, 28, heading_font, ui::ink);
                ui::rounded(dc, rect(704 + delta / 2, 430, 34, 34), px(9), ui::teal_soft);
                ui::icon(dc, ui::Icon::Mic, rect(712 + delta / 2, 438, 18, 18), ui::teal);
                txt(L"人声音量", 750 + delta / 2, 434, 160, 28, heading_font, ui::ink);
                card(252, 594, 836 + delta, 86);
                txt(L"让讲话更突出，音乐平滑降低后恢复", 276, 644, 420, 22, small_font, ui::muted);
                card(252, 700, 836 + delta, 100);
                txt(L"输出电平", 604 + delta, 719, 150, 20, small_font, ui::muted);
            } else if (page == 1) {
                txt(L"录一段，听听效果", 252, 122, 480, 26, heading_font, ui::ink);
                card(252, 166, 836 + delta, 180);
                ui::rounded(dc, rect(276, 185, 34, 34), px(9), ui::orange_soft);
                ui::icon(dc, ui::Icon::Record, rect(284, 193, 18, 18), ui::orange);
                txt(L"本地录音", 322, 188, 400, 28, heading_font, ui::ink);
                txt(L"录制 15 秒自动保存，也可以提前停止。文件保存在桌面的 PartyMic录音 文件夹。", 276, 296,
                    780 + delta, 26, small_font, ui::muted);
                card(252, 366, 836 + delta, 136);
                txt(L"人声音量", 276, 382, 300, 28, heading_font, ui::ink);
                txt(L"与混音共用，可边录边调", 276, 469, 500, 20, small_font, ui::muted);
                card(252, 522, 836 + delta, 178);
                ui::icon(dc, ui::Icon::Headphones, rect(276, 545, 21, 21), ui::teal);
                txt(L"试听回放", 310, 541, 400, 28, heading_font, ui::ink);
                card(252, 720, 836 + delta, 80);
                txt(L"录放电平", 276, 733, 90, 20, small_font, ui::muted);
            } else {
                card(252, 138, 836 + delta, 318);
                txt(L"三步，开始你的混音", 276, 158, 700, 32, heading_font, ui::ink);
                const wchar_t *steps[] = {L"01   选择音乐软件和麦克风", L"02   调好音乐与人声的比例",
                                          L"03   在语音软件中选择 CABLE Output"};
                const wchar_t *hints[] = {L"让网易云先播放，再刷新设备；虚拟声卡选择 CABLE Input。",
                                          L"音乐默认 25%，人声默认 100%。自动压低可按需要开启。",
                                          L"点击开始混音，音乐与讲话就会一起送入虚拟麦克风。"};
                for (int i = 0; i < 3; ++i) {
                    txt(steps[i], 276, 216 + i * 72, 760 + delta, 26, font, ui::ink);
                    txt(hints[i], 312, 245 + i * 72, 720 + delta, 24, small_font, ui::muted);
                }
                card(252, 476, 836 + delta, 146);
                txt(L"先录音，再试听", 276, 496, 700, 30, heading_font, ui::ink);
                txt(L"仅人声录音不需要开始混音。试听输出请选择正在使用的 HDMI、耳机或音响。", 276, 540,
                    780 + delta, 24, font, ui::muted);
                txt(L"试听会先停止混音，避免声音循环；暂停网易云后比较，更容易听清细节。", 276, 573, 780 + delta,
                    24, font, ui::muted);
                ui::rounded(dc, rect(252, 642, 836 + delta, 94), px(12), ui::teal_soft);
                txt(L"佩戴耳机，保留更干净的背景音乐", 276, 657, 760 + delta, 28, heading_font, ui::teal_text);
                txt(L"音响外放可能被麦克风再次拾回。游戏与 ACE 兼容性尚未经认证。", 276, 693, 760 + delta, 24,
                    small_font, ui::teal_text);
            }
        }
        RestoreDC(dc, content_clip);
        BitBlt(target, 0, 0, bounds.right, bounds.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    void create() {
        dpi = GetDpiForWindow(window);
        ui::refresh_motion_settings();
        update_icons();
        nav_mix = button(L"混音工作台", 14, 154, 192, 44, NavMix, 7, ui::Kind::Nav, ui::Icon::Mix);
        nav_record = button(L"录音与试听", 14, 206, 192, 44, NavRecord, 7, ui::Kind::Nav, ui::Icon::Mic);
        nav_guide = button(L"使用指南", 14, 258, 192, 44, NavGuide, 7, ui::Kind::Nav, ui::Icon::Book);
        ui::selected(nav_mix, true);
        refresh_button =
            button(L"刷新设备", 952, 120, 136, 32, Refresh, 1, ui::Kind::Neutral, ui::Icon::Refresh, Right);
        caption(L"音乐软件", 308, 227, 128);
        caption(L"实体麦克风", 308, 277, 128);
        caption(L"虚拟声卡", 308, 327, 128);
        music_combo = combo(444, 215, 620, Music);
        mic_combo = combo(444, 265, 620, Microphone);
        cable_combo = combo(444, 315, 620, Cable);
        music_slider = slider(268, 478, 376, MusicVolume, 0, 200, 25, 1, HalfWidth, ui::orange);
        mic_slider = slider(696, 478, 376, MicVolume, 0, 200, 100, 3, SecondColumn);
        music_value = caption(L"25%", 550, 430, 90, 38, 1, ui::ink, SecondRight, 2);
        mic_value = caption(L"100%", 978, 430, 90, 38, 3, ui::ink, Right, 2);
        mute_music = check(L"静音音乐", 276, 529, 160, MuteMusic);
        mute_mic = check(L"静音麦克风", 704, 529, 170, MuteMic, 3, SecondRight);
        music_meter = meter(450, 535, 190, 1, ui::orange, HalfWidth);
        mic_meter = meter(890, 535, 174, 1, ui::teal, SecondColumn);
        duck = check(L"说话时自动压低音乐", 276, 608, 380, Ducking);
        caption(L"保留", 706, 615, 56, 28, 1, ui::muted, Right);
        duck_slider = slider(762, 610, 226, DuckFloor, 25, 100, 65, 1, Right);
        duck_value = caption(L"65%", 1002, 615, 62, 28, 1, ui::teal_text, Right);
        status = caption(L"", 276, 716, 304, 42, 1, ui::ink);
        detail = caption(L"", 276, 768, 770, 20, 1, ui::muted, Stretch, -1);
        output_meter = meter(604, 745, 150, 1, ui::teal, Right);
        stop = button(L"停止", 788, 717, 116, 42, Stop, 1, ui::Kind::Neutral, ui::Icon::Stop, Right);
        start = button(L"开始混音", 916, 717, 148, 42, Start, 1, ui::Kind::Primary, ui::Icon::Play, Right);
        record_source = combo(276, 234, 444, RecordSource, 2);
        item(record_source, L"仅人声 · 无需开始混音");
        item(record_source, L"音乐 + 人声 · 录制实际混音");
        SendMessageW(record_source, CB_SETCURSEL, 0, 0);
        record_button =
            button(L"录制 15 秒", 916, 234, 148, 40, Record, 2, ui::Kind::Primary, ui::Icon::Record, Right);
        preview_stop = button(L"停止录音 / 试听", 736, 234, 164, 40, StopPreview, 2, ui::Kind::Neutral,
                              ui::Icon::Stop, Right);
        preview_output = combo(276, 583, 624, PreviewOutput, 2);
        play_button = button(L"试听刚才录音", 916, 583, 148, 40, PlayPreview, 2, ui::Kind::Primary,
                             ui::Icon::Play, Right);
        caption(L"试听音量", 276, 650, 96, 28, 2);
        preview_slider = slider(374, 645, 344, PreviewVolume, 0, 100, 70, 2, Stretch);
        preview_value = caption(L"70%", 734, 650, 100, 28, 2, ui::ink, Right);
        open_recordings = button(L"打开文件夹", 916, 641, 148, 38, OpenRecordings, 2, ui::Kind::Neutral,
                                 ui::Icon::Folder, Right);
        preview_meter = meter(372, 736, 200, 2);
        preview_status = caption(L"", 276, 761, 788, 32, 2, ui::muted, Stretch, -1);
        layout();
        refresh();
        update();
        SetTimer(window, 1, 80, nullptr);
    }
    static int selection(HWND combo) { return int(SendMessageW(combo, CB_GETCURSEL, 0, 0)); }
    static void item(HWND combo, const std::wstring &name) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    static void set_text(HWND h, const std::wstring &text) {
        const int n = GetWindowTextLengthW(h);
        std::wstring current(size_t(n) + 1, L'\0');
        GetWindowTextW(h, current.data(), n + 1);
        current.resize(size_t(n));
        if (current != text)
            SetWindowTextW(h, text.c_str());
    }
    void refresh() {
        DWORD old_pid = 0;
        std::wstring old_mic, old_cable, old_speaker;
        const auto default_mic = default_endpoint_id(eCapture, eCommunications);
        int index = selection(music_combo);
        if (index >= 0 && static_cast<std::size_t>(index) < applications.size())
            old_pid = applications[index].pid;
        index = selection(mic_combo);
        if (index >= 0 && static_cast<std::size_t>(index) < microphones.size())
            old_mic = microphones[index].id;
        index = selection(cable_combo);
        if (index >= 0 && static_cast<std::size_t>(index) < cables.size())
            old_cable = cables[index].id;
        index = selection(preview_output);
        if (index >= 0 && static_cast<std::size_t>(index) < speakers.size())
            old_speaker = speakers[index].id;
        // Read everything before replacing the visible list, preserving it on failure.
        auto next_apps = audio_applications();
        auto next_mics = endpoints(eCapture);
        auto renders = endpoints(eRender);
        std::vector<Endpoint> next_cables, next_speakers;
        for (const auto &endpoint : renders)
            if (is_virtual_cable(endpoint.name))
                next_cables.push_back(endpoint);
        for (const auto &endpoint : renders) {
            auto n = endpoint.name;
            std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return std::towlower(c); });
            if (n.find(L"vb-audio") == std::wstring::npos && n.find(L"steam streaming") == std::wstring::npos)
                next_speakers.push_back(endpoint);
        }
        std::erase_if(next_mics, [](const auto &device) {
            auto n = device.name;
            std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return std::towlower(c); });
            return n.find(L"vb-audio") != std::wstring::npos;
        });
        applications = std::move(next_apps);
        microphones = std::move(next_mics);
        cables = std::move(next_cables);
        speakers = std::move(next_speakers);
        for (HWND combo : {music_combo, mic_combo, cable_combo, preview_output})
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        int app_index = 0, mic_index = 0, cable_index = 0, speaker_index = 0;
        for (std::size_t i = 0; i < applications.size(); ++i) {
            const auto &app = applications[i];
            item(music_combo, app.label);
            if (old_pid ? app.pid == old_pid : _wcsicmp(app.executable.c_str(), L"cloudmusic.exe") == 0)
                app_index = static_cast<int>(i);
        }
        for (std::size_t i = 0; i < microphones.size(); ++i) {
            item(mic_combo, microphones[i].name);
            if (microphones[i].id == (old_mic.empty() ? default_mic : old_mic))
                mic_index = static_cast<int>(i);
        }
        for (std::size_t i = 0; i < cables.size(); ++i) {
            item(cable_combo, cables[i].name);
            if (cables[i].id == old_cable)
                cable_index = static_cast<int>(i);
        }
        const auto default_speaker = default_endpoint_id(eRender, eMultimedia);
        for (std::size_t i = 0; i < speakers.size(); ++i) {
            item(preview_output, speakers[i].name);
            if (speakers[i].id == (old_speaker.empty() ? default_speaker : old_speaker))
                speaker_index = static_cast<int>(i);
        }
        SendMessageW(preview_output, CB_SETCURSEL, speaker_index, 0);
        SendMessageW(music_combo, CB_SETCURSEL, app_index, 0);
        SendMessageW(mic_combo, CB_SETCURSEL, mic_index, 0);
        SendMessageW(cable_combo, CB_SETCURSEL, cable_index, 0);
    }
    void read_settings() {
        page_transition.stop();
        page_moving = false;
        MixSettings settings;
        const auto music = SendMessageW(music_slider, TBM_GETPOS, 0, 0);
        const auto mic = SendMessageW(mic_slider, TBM_GETPOS, 0, 0);
        settings.music_gain = static_cast<float>(music) / 100;
        settings.mic_gain = static_cast<float>(mic) / 100;
        settings.music_muted = SendMessageW(mute_music, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings.mic_muted = SendMessageW(mute_mic, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings.ducking = SendMessageW(duck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings.duck_floor = static_cast<float>(SendMessageW(duck_slider, TBM_GETPOS, 0, 0)) / 100;
        preview.microphone(settings.mic_gain, settings.mic_muted);
        const auto playback = SendMessageW(preview_slider, TBM_GETPOS, 0, 0);
        preview.playback_gain(static_cast<float>(playback) / 100);
        set_text(preview_value, (std::to_wstring(playback) + L"%").c_str());
        set_text(duck_value,
                 (std::to_wstring(static_cast<int>(settings.duck_floor * 100 + 0.5f)) + L"%").c_str());
        engine.settings(settings);
        set_text(music_value, (std::to_wstring(music) + L"%").c_str());
        set_text(mic_value, (std::to_wstring(mic) + L"%").c_str());
    }
    void command(int id, int notification) {
        // Focus notifications can be sent synchronously while preparing a new
        // page. Only actual clicks/selections should dismiss its transition.
        if ((id < NavMix || id > NavGuide) && (notification == BN_CLICKED || notification == CBN_SELCHANGE)) {
            page_transition.stop();
            page_moving = false;
        }
        switch (id) {
        case NavMix:
            navigate(0);
            break;
        case NavRecord:
            navigate(1);
            break;
        case NavGuide:
            navigate(2);
            break;
        case Refresh:
            if (!engine.active() && !preview.busy())
                refresh();
            break;
        case MuteMusic:
        case MuteMic:
        case Ducking:
            read_settings();
            break;
        case Start: {
            if (preview.busy())
                return;
            int a = selection(music_combo), m = selection(mic_combo), c = selection(cable_combo);
            if (a < 0 || m < 0 || c < 0 || static_cast<std::size_t>(a) >= applications.size() ||
                static_cast<std::size_t>(m) >= microphones.size() ||
                static_cast<std::size_t>(c) >= cables.size()) {
                MessageBoxW(window,
                            L"请先播放音乐并刷新，选择音乐软件、实体麦克风和 CABLE "
                            L"Input。\n没有虚拟声卡时，请先安装 VB-CABLE 官方驱动。",
                            L"缺少音源或虚拟声卡", MB_OK | MB_ICONINFORMATION);
                return;
            }
            read_settings();
            engine.start({applications[a].pid, applications[a].executable, microphones[m].id, cables[c].id});
            break;
        }
        case Stop:
            if (preview.recording())
                preview.stop();
            engine.stop();
            break;
        case Record: {
            if (preview.busy())
                return;
            const bool mixed = selection(record_source) == 1;
            std::wstring capture_id;
            if (mixed) {
                if (!engine.active()) {
                    MessageBoxW(window, L"录制混音前请先开始混音。仅人声录音无需音乐或虚拟声卡。",
                                L"录制混音", MB_OK);
                    return;
                }
                const auto c = selection(cable_combo);
                if (c < 0 || static_cast<std::size_t>(c) >= cables.size())
                    return;
                // Only the official standard stereo VB-CABLE pair is supported.
                for (const auto &e : endpoints(eCapture))
                    if (e.name.find(L"CABLE Output") != std::wstring::npos) {
                        capture_id = e.id;
                        break;
                    }
            } else {
                const auto m = selection(mic_combo);
                if (m >= 0 && static_cast<std::size_t>(m) < microphones.size())
                    capture_id = microphones[m].id;
            }
            if (capture_id.empty()) {
                MessageBoxW(window, L"未找到可录制的设备，请刷新设备后重试。", L"录音设备", MB_OK);
                return;
            }
            read_settings();
            preview.record(capture_id, mixed);
            break;
        }
        case StopPreview:
            preview.stop();
            break;
        case PlayPreview: {
            if (preview.busy())
                return;
            const auto p = selection(preview_output);
            if (p < 0 || static_cast<std::size_t>(p) >= speakers.size()) {
                MessageBoxW(window, L"请选择耳机、HDMI 或扬声器作为试听输出。", L"试听输出", MB_OK);
                return;
            }
            engine.stop();
            read_settings();
            preview.play(speakers[p].id);
            break;
        }
        case OpenRecordings: {
            const auto file = preview.last_file();
            if (!file.empty())
                ShellExecuteW(window, L"open", file.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        }
        update();
    }
    void update() {
        const bool active = engine.active();
        if (active != previous_active) {
            previous_active = active;
            begin_badge_motion();
            InvalidateRect(window, nullptr, FALSE);
        }
        const bool preview_busy = preview.busy();
        static int old_preview_mode = 0;
        const int mode = preview.recording() ? 1 : (preview.playing() ? 2 : 0);
        if (old_preview_mode != mode) {
            old_preview_mode = mode;
            begin_badge_motion();
            RECT badge{px(logical_width() - 168), px(30), px(logical_width() - 24), px(72)};
            InvalidateRect(window, &badge, FALSE);
        }
        for (HWND control : {music_combo, mic_combo, cable_combo, refresh_button, start})
            EnableWindow(control, !active && !preview_busy);
        EnableWindow(stop, active);
        EnableWindow(record_source, !preview_busy);
        EnableWindow(record_button, !preview_busy && !microphones.empty());
        EnableWindow(preview_stop, preview_busy);
        EnableWindow(play_button, !preview_busy && !preview.last_file().empty() && !speakers.empty());
        EnableWindow(preview_output, !preview_busy);
        EnableWindow(open_recordings, !preview.last_file().empty());
        EnableWindow(duck_slider, SendMessageW(duck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        set_text(preview_status, preview.status().c_str());
        std::wstring message = engine.status();
        if (active)
            message = L"正在混音 → CABLE Output";
        else if (cables.empty())
            message = L"请安装 VB-CABLE 后刷新。";
        else if (applications.empty())
            message = L"请先播放音乐，再刷新设备。";
        else if (message.find(L"已停止。") == 0)
            message = L"混音已停止，声音不再传入。";
        else if (message.find(L"就绪") != std::wstring::npos)
            message = L"准备就绪，点击开始混音。";
        set_text(status, message.c_str());
        const auto meter = [](float value) {
            if (value <= 0.001f)
                return 0;
            return static_cast<int>(std::clamp((20 * std::log10(value) + 60) / 60, 0.0f, 1.0f) * 100);
        };
        SendMessageW(preview_meter, PBM_SETPOS, meter(preview.level()), 0);
        SendMessageW(music_meter, PBM_SETPOS, meter(engine.music_level()), 0);
        SendMessageW(mic_meter, PBM_SETPOS, meter(engine.mic_level()), 0);
        SendMessageW(output_meter, PBM_SETPOS, meter(engine.output_level()), 0);
        if (active) {
            auto text = std::wstring(L"音乐保留 ") +
                        std::to_wstring(static_cast<int>(engine.duck_gain() * 100)) +
                        L"%  ·  在语音软件中选择 CABLE Output";
            set_text(detail, text.c_str());
        } else
            set_text(detail, L"准备好后点击开始混音，在语音软件中选择 CABLE Output。");
    }
};
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(window, message, wparam, lparam);
    try {
        switch (message) {
        case WM_CREATE:
            app->create();
            return 0;
        case WM_COMMAND:
            app->command(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_HSCROLL:
            app->read_settings();
            return 0;
        case WM_TIMER:
            if (wparam == 2)
                app->motion_tick();
            else if (wparam == 1)
                app->update();
            return 0;
        case WM_SIZE:
            app->page_transition.stop();
            app->page_moving = false;
            if (wparam == SIZE_MINIMIZED)
                app->stop_motion();
            app->position_controls();
            return 0;
        case WM_SETTINGCHANGE:
            ui::refresh_motion_settings();
            if (!ui::motion_enabled()) {
                app->stop_motion();
                app->position_controls();
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
            info->ptMinTrackSize = {app->px(1000), app->px(650)};
            return 0;
        }
        case WM_MEASUREITEM: {
            auto *item = reinterpret_cast<MEASUREITEMSTRUCT *>(lparam);
            item->itemHeight = app->px(34);
            return TRUE;
        }
        case WM_DRAWITEM: {
            auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(lparam);
            if (item->CtlType == ODT_COMBOBOX) {
                ui::draw_combo_item(*item, app->font);
                return TRUE;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(window, &ps);
            app->paint(dc);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            app->paint(reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_VSCROLL:
            app->scroll(LOWORD(wparam));
            return 0;
        case WM_MOUSEWHEEL: {
            const POINT pointer{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            const auto hovered = WindowFromPoint(pointer);
            for (auto slider : {app->music_slider, app->mic_slider, app->duck_slider, app->preview_slider}) {
                if (hovered == slider && IsWindowEnabled(slider))
                    return SendMessageW(slider, message, wparam, lparam);
            }
            app->wheel_remainder += static_cast<short>(HIWORD(wparam));
            const int steps = app->wheel_remainder / WHEEL_DELTA;
            app->wheel_remainder %= WHEEL_DELTA;
            if (steps)
                app->scroll(-1, steps);
            return 0;
        }
        case WM_DPICHANGED: {
            app->stop_motion();
            app->dpi = HIWORD(wparam);
            auto *rect = reinterpret_cast<RECT *>(lparam);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            app->update_icons();
            app->layout();
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            auto dc = reinterpret_cast<HDC>(wparam);
            const auto color =
                reinterpret_cast<ULONG_PTR>(GetPropW(reinterpret_cast<HWND>(lparam), L"PMTextColor"));
            SetTextColor(dc, color ? COLORREF(color - 1) : ui::ink);
            SetBkColor(dc, ui::white);
            return reinterpret_cast<LRESULT>(app->background);
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            app->preview.stop();
            app->engine.stop();
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            KillTimer(window, 2);
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception &error) {
        MessageBoxA(window, error.what(), "PartyMic", MB_OK | MB_ICONERROR);
        if (message == WM_CREATE)
            return -1;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized))
        return 1;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartupInput startup;
    if (Gdiplus::GdiplusStartup(&gdiplus, &startup, nullptr) != Gdiplus::Ok) {
        CoUninitialize();
        return 1;
    }
    int exit_code = 0;
    try {
        INITCOMMONCONTROLSEX common{sizeof(common),
                                    ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&common);
        App app;
        WNDCLASSW type{};
        type.lpfnWndProc = procedure;
        type.hInstance = instance;
        type.lpszClassName = L"PartyMicWindow";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        if (!RegisterClassW(&type))
            throw std::runtime_error("Cannot register window");
        const UINT dpi = GetDpiForSystem();
        RECT rect{0, 0, MulDiv(design_width, static_cast<int>(dpi), 96),
                  MulDiv(820, static_cast<int>(dpi), 96)};
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                                WS_THICKFRAME | WS_VSCROLL | WS_CLIPCHILDREN;
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
        RECT work{};
        int height = static_cast<int>(rect.bottom - rect.top);
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
            height = std::min(height, static_cast<int>(work.bottom - work.top));
        HWND window = CreateWindowExW(0, type.lpszClassName, L"PartyMic · 开黑背景音乐", style,
                                      CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, height, nullptr,
                                      nullptr, instance, &app);
        if (!window)
            throw std::runtime_error("Cannot create PartyMic window");
        // A system-DPI-unaware launcher or a different monitor can make the
        // initial system DPI differ from this per-monitor-aware window's DPI.
        RECT actual{0, 0, app.px(design_width), app.px(820)};
        AdjustWindowRectExForDpi(&actual, style, FALSE, 0, app.dpi);
        int actual_height = static_cast<int>(actual.bottom - actual.top);
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
            actual_height = std::min(actual_height, static_cast<int>(work.bottom - work.top));
        SetWindowPos(window, nullptr, 0, 0, actual.right - actual.left, actual_height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(window, show);
        MSG message{};
        int result;
        while ((result = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (result == -1)
            exit_code = 1;
    } catch (const std::exception &error) {
        MessageBoxA(nullptr, error.what(), "PartyMic startup failed", MB_OK | MB_ICONERROR);
        exit_code = 1;
    }
    Gdiplus::GdiplusShutdown(gdiplus);
    CoUninitialize();
    return exit_code;
}
