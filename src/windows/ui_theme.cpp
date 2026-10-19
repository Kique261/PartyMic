#include "windows/ui_theme.h"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <string>

namespace partymic::ui {
namespace {
Gdiplus::Color color(COLORREF c) { return {255, GetRValue(c), GetGValue(c), GetBValue(c)}; }
bool allow_motion = true;
constexpr UINT_PTR motion_timer = 0x504d;
struct Tween {
    float from = 0, to = 0;
    ULONGLONG started = 0;
    unsigned duration = 160;
    float value(ULONGLONG now) const {
        const float t = std::clamp(float(now - started) / duration, 0.0f, 1.0f);
        const float eased = 1 - (1 - t) * (1 - t) * (1 - t);
        return from + (to - from) * eased;
    }
    bool active(ULONGLONG now) const { return from != to && now - started < duration; }
    void target(float next, ULONGLONG now, unsigned ms = 160) {
        if (next == to)
            return;
        from = value(now);
        to = next;
        started = now;
        duration = ms;
    }
    void finish() { from = to; }
};
COLORREF blend(COLORREF a, COLORREF b, float t) {
    return RGB(int(GetRValue(a) + (int(GetRValue(b)) - GetRValue(a)) * t),
               int(GetGValue(a) + (int(GetGValue(b)) - GetGValue(a)) * t),
               int(GetBValue(a) + (int(GetBValue(b)) - GetBValue(a)) * t));
}
struct State {
    Kind kind;
    Icon shape;
    COLORREF accent;
    bool hover = false, selected = false, timer_running = false;
    Tween hover_motion, selection_motion, check_motion, press_motion, level_motion;
    int wheel_remainder = 0;
    State(Kind k, Icon i, COLORREF c) : kind(k), shape(i), accent(c) {}
    bool active(ULONGLONG now) const {
        return hover_motion.active(now) || selection_motion.active(now) || check_motion.active(now) ||
               press_motion.active(now) || level_motion.active(now);
    }
    void finish() {
        for (auto *t : {&hover_motion, &selection_motion, &check_motion, &press_motion, &level_motion})
            t->finish();
    }
};
void animate(HWND h, State &s) {
    const auto now = GetTickCount64();
    const bool enabled = IsWindowEnabled(h);
    s.hover_motion.target(s.hover && enabled ? 1.0f : 0.0f, now);
    s.selection_motion.target(s.selected ? 1.0f : 0.0f, now, 180);
    if (s.kind == Kind::Check)
        s.check_motion.target(SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1.0f : 0.0f, now, 180);
    if (s.kind == Kind::Primary || s.kind == Kind::Neutral || s.kind == Kind::Nav)
        s.press_motion.target(enabled && (SendMessageW(h, BM_GETSTATE, 0, 0) & BST_PUSHED) ? 1.0f : 0.0f, now,
                              80);
    if (s.kind == Kind::Meter) {
        const float next = std::clamp(float(SendMessageW(h, PBM_GETPOS, 0, 0)) / 100, 0.0f, 1.0f);
        s.level_motion.target(next, now, next > s.level_motion.value(now) ? 80 : 240);
    }
    if (!allow_motion || !IsWindowVisible(h) || IsIconic(GetAncestor(h, GA_ROOT)))
        s.finish();
    if (s.active(now)) {
        if (!s.timer_running)
            s.timer_running = SetTimer(h, motion_timer, 16, nullptr) != 0;
        if (!s.timer_running)
            s.finish();
    } else if (s.timer_running) {
        KillTimer(h, motion_timer);
        s.timer_running = false;
    }
    InvalidateRect(h, nullptr, FALSE);
}
void setup(Gdiplus::Graphics &g) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
}
void path(Gdiplus::GraphicsPath &p, RECT r, float radius) {
    const float d = std::min({radius * 2, float(r.right - r.left), float(r.bottom - r.top)});
    const float x = float(r.left) + 0.5f, y = float(r.top) + 0.5f, w = float(r.right - r.left) - 1,
                h = float(r.bottom - r.top) - 1;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}
std::wstring label(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    return s;
}
int scale(HWND h, int n) { return MulDiv(n, GetDpiForWindow(h), 96); }
void paint(HWND h, HDC dc, State &state) {
    RECT r{};
    GetClientRect(h, &r);
    const int w = r.right, hh = r.bottom, p = scale(h, 12);
    const bool enabled = IsWindowEnabled(h);
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0));
    const COLORREF fg = enabled ? ink : RGB(161, 161, 170);
    HBRUSH bg = CreateSolidBrush(state.kind == Kind::Nav ? sidebar : white);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    const bool focus = GetFocus() == h;
    const auto now = GetTickCount64();
    const float hover = state.hover_motion.value(now), selection = state.selection_motion.value(now),
                checked_amount = state.check_motion.value(now), pressed = state.press_motion.value(now);
    if (state.kind == Kind::Slider) {
        const auto lo = SendMessageW(h, TBM_GETRANGEMIN, 0, 0), hi = SendMessageW(h, TBM_GETRANGEMAX, 0, 0),
                   pos = SendMessageW(h, TBM_GETPOS, 0, 0);
        RECT channel{}, thumb{};
        SendMessageW(h, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        SendMessageW(h, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        const int left = std::max(scale(h, 10), static_cast<int>(channel.left)),
                  right = std::min(w - scale(h, 10), static_cast<int>(channel.right));
        const int cy = hh / 2;
        RECT track{left, cy - scale(h, 2), right, cy + scale(h, 2)};
        rounded(dc, track, scale(h, 3), subtle);
        const float ratio = hi > lo ? std::clamp(float(pos - lo) / float(hi - lo), 0.0f, 1.0f) : 0;
        const int x = left + int((right - left) * ratio);
        RECT fill = track;
        fill.right = x;
        if (fill.right > fill.left)
            rounded(dc, fill, scale(h, 3), enabled ? state.accent : strong);
        Gdiplus::Graphics g(dc);
        setup(g);
        const float radius = float(scale(h, 7)) + hover * scale(h, 1);
        Gdiplus::SolidBrush b(color(enabled ? state.accent : strong));
        g.FillEllipse(&b, float(x) - radius, float(cy) - radius, radius * 2, radius * 2);
        if (focus) {
            Gdiplus::Pen pen(color(state.accent), 1);
            g.DrawEllipse(&pen, float(x) - radius - 3, float(cy) - radius - 3, radius * 2 + 6,
                          radius * 2 + 6);
        }
        return;
    }
    if (state.kind == Kind::Meter) {
        const float value = state.level_motion.value(now);
        RECT bar{0, hh / 2 - scale(h, 3), w, hh / 2 + scale(h, 3)};
        rounded(dc, bar, scale(h, 4), subtle);
        bar.right = int(w * value);
        if (bar.right > 1)
            rounded(dc, bar, scale(h, 4), state.accent);
        return;
    }
    if (state.kind == Kind::Combo) {
        rounded(dc, r, scale(h, 8), enabled ? white : RGB(250, 250, 251),
                focus ? state.accent : blend(border, strong, hover));
        auto s = label(h);
        if (s.empty())
            s = L"请选择设备";
        RECT tr{p, 0, w - scale(h, 34), hh};
        text(dc, s, tr, font, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Gdiplus::Graphics g(dc);
        setup(g);
        Gdiplus::Pen pen(color(muted), float(scale(h, 1)) + 0.3f);
        const float x = float(w - scale(h, 21)), y = hh / 2.0f;
        g.DrawLine(&pen, x - 4, y - 2, x, y + 2);
        g.DrawLine(&pen, x, y + 2, x + 4, y - 2);
        return;
    }
    if (state.kind == Kind::Check) {
        const bool checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool toggle = GetDlgCtrlID(h) == 109;
        const int width = scale(h, toggle ? 34 : 17), height = scale(h, toggle ? 20 : 17);
        const int top = (hh - height) / 2;
        RECT box{1, top, width, top + height};
        rounded(dc, box, scale(h, toggle ? 10 : 4), blend(white, enabled ? teal : strong, checked_amount),
                blend(strong, enabled ? teal : strong, checked_amount));
        if (toggle) {
            Gdiplus::Graphics g(dc);
            setup(g);
            Gdiplus::SolidBrush b(color(blend(strong, white, checked_amount)));
            const int d = height - scale(h, 6),
                      x = scale(h, 3) + int((width - d - scale(h, 6)) * checked_amount);
            g.FillEllipse(&b, float(x), float(top + scale(h, 3)), float(d), float(d));
        } else if (checked) {
            Gdiplus::Graphics g(dc);
            setup(g);
            Gdiplus::Pen pen(color(white), float(scale(h, 2)));
            g.DrawLine(&pen, float(scale(h, 4)), float(top + height / 2), float(scale(h, 7)),
                       float(top + height / 2 + scale(h, 3)));
            g.DrawLine(&pen, float(scale(h, 7)), float(top + height / 2 + scale(h, 3)), float(scale(h, 13)),
                       float(top + scale(h, 5)));
        }
        RECT tr{width + scale(h, 9), 0, w, hh};
        text(dc, label(h), tr, font, fg);
        if (focus) {
            RECT fr{width + scale(h, 5), 2, w - 2, hh - 2};
            DrawFocusRect(dc, &fr);
        }
        return;
    }
    COLORREF fill = white, stroke = strong, foreground = fg;
    if (state.kind == Kind::Primary) {
        fill = enabled ? blend(blend(orange, orange_hover, hover), RGB(213, 84, 7), pressed) : subtle;
        stroke = CLR_INVALID;
        foreground = enabled ? white : muted;
    } else if (state.kind == Kind::Nav) {
        fill = blend(blend(sidebar, RGB(235, 235, 240), hover), teal_soft, selection);
        stroke = CLR_INVALID;
        foreground = blend(fg, teal_text, selection);
    } else if (enabled)
        fill = blend(white, subtle, hover);
    rounded(dc, r, scale(h, state.kind == Kind::Nav ? 9 : 8), fill, focus ? teal : stroke);
    int ix = state.kind == Kind::Nav ? scale(h, 14) : p;
    RECT tr{0, 0, w, hh};
    if (state.shape != Icon::None) {
        const int sz = scale(h, 18);
        RECT ir{ix, (hh - sz) / 2, ix + sz, (hh + sz) / 2};
        icon(dc, state.shape, ir, foreground);
        tr.left = ix + sz + scale(h, 8);
        tr.right = w - p;
    }
    OffsetRect(&tr, 0, int(pressed * scale(h, 1) + 0.5f));
    text(dc, label(h), tr, font, foreground,
         (state.kind == Kind::Nav ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE);
}
// Keep the native trackbar value/accessibility, but own its input painting too.
// Native trackbar input can draw directly to its DC, bypassing our WM_PAINT buffer.
void slider_value(HWND h, int value, int notification) {
    const int lo = int(SendMessageW(h, TBM_GETRANGEMIN, 0, 0)),
              hi = int(SendMessageW(h, TBM_GETRANGEMAX, 0, 0));
    value = std::clamp(value, lo, hi);
    SendMessageW(h, TBM_SETPOS, FALSE, value);
    SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(notification, value), reinterpret_cast<LPARAM>(h));
    InvalidateRect(h, nullptr, FALSE);
}
void slider_pointer(HWND h, LPARAM point) {
    RECT channel{}, r{};
    GetClientRect(h, &r);
    SendMessageW(h, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    const int left = std::max(scale(h, 10), int(channel.left)),
              right = std::min(int(r.right) - scale(h, 10), int(channel.right));
    const float ratio =
        std::clamp(float(static_cast<short>(LOWORD(point)) - left) / std::max(1, right - left), 0.0f, 1.0f);
    const int lo = int(SendMessageW(h, TBM_GETRANGEMIN, 0, 0)),
              hi = int(SendMessageW(h, TBM_GETRANGEMAX, 0, 0));
    slider_value(h, lo + int(std::lround(ratio * (hi - lo))), TB_THUMBTRACK);
}
LRESULT CALLBACK subclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    auto *s = reinterpret_cast<State *>(data);
    if (m == WM_NCDESTROY) {
        KillTimer(h, motion_timer);
        RemoveWindowSubclass(h, subclass, 1);
        delete s;
        return DefSubclassProc(h, m, w, l);
    }
    if (m == WM_TIMER && w == motion_timer) {
        if (!allow_motion || !IsWindowVisible(h) || IsIconic(GetAncestor(h, GA_ROOT)))
            s->finish();
        if (!s->active(GetTickCount64())) {
            KillTimer(h, motion_timer);
            s->timer_running = false;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (m == WM_MOUSEMOVE && !s->hover) {
        s->hover = true;
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        animate(h, *s);
    }
    if (m == WM_MOUSELEAVE) {
        s->hover = false;
        animate(h, *s);
    }
    if (s->kind == Kind::Slider && IsWindowEnabled(h)) {
        if (m == WM_LBUTTONDOWN) {
            SetFocus(h);
            SetCapture(h);
            slider_pointer(h, l);
            return 0;
        }
        if (m == WM_MOUSEMOVE && GetCapture() == h) {
            slider_pointer(h, l);
            return 0;
        }
        if (m == WM_LBUTTONUP && GetCapture() == h) {
            slider_pointer(h, l);
            ReleaseCapture();
            SendMessageW(GetParent(h), WM_HSCROLL, TB_ENDTRACK, reinterpret_cast<LPARAM>(h));
            return 0;
        }
        if (m == WM_MOUSEWHEEL) {
            s->wheel_remainder += static_cast<short>(HIWORD(w));
            const int steps = s->wheel_remainder / WHEEL_DELTA;
            s->wheel_remainder %= WHEEL_DELTA;
            if (steps)
                slider_value(h, int(SendMessageW(h, TBM_GETPOS, 0, 0)) + steps, TB_THUMBPOSITION);
            return 0;
        }
        if (m == WM_KEYDOWN) {
            int value = int(SendMessageW(h, TBM_GETPOS, 0, 0));
            const int step = std::max(1, int(SendMessageW(h, TBM_GETLINESIZE, 0, 0))),
                      page = std::max(1, int(SendMessageW(h, TBM_GETPAGESIZE, 0, 0)));
            switch (w) {
            case VK_RIGHT:
            case VK_UP:
                value += step;
                break;
            case VK_LEFT:
            case VK_DOWN:
                value -= step;
                break;
            case VK_PRIOR:
                value += page;
                break;
            case VK_NEXT:
                value -= page;
                break;
            case VK_HOME:
                value = int(SendMessageW(h, TBM_GETRANGEMIN, 0, 0));
                break;
            case VK_END:
                value = int(SendMessageW(h, TBM_GETRANGEMAX, 0, 0));
                break;
            default:
                return DefSubclassProc(h, m, w, l);
            }
            slider_value(h, value, TB_THUMBPOSITION);
            return 0;
        }
    }
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_PAINT || m == WM_PRINTCLIENT || m == WM_PRINT) {
        PAINTSTRUCT ps{};
        HDC target = m == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
        RECT r{};
        GetClientRect(h, &r);
        HDC buffer = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(1L, r.right), std::max(1L, r.bottom));
        auto old = SelectObject(buffer, bitmap);
        paint(h, buffer, *s);
        BitBlt(target, 0, 0, r.right, r.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        if (m == WM_PAINT)
            EndPaint(h, &ps);
        return 0;
    }
    // Do not redraw settled meters at the 80 ms audio polling interval.
    if (s->kind == Kind::Meter && m == PBM_SETPOS && SendMessageW(h, PBM_GETPOS, 0, 0) == LRESULT(w))
        return LRESULT(w);
    const auto result = DefSubclassProc(h, m, w, l);
    if (m == WM_ENABLE || m == WM_SETFOCUS || m == WM_KILLFOCUS || m == WM_SETTEXT || m == WM_LBUTTONDOWN ||
        m == WM_LBUTTONUP || m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CAPTURECHANGED ||
        (s->kind == Kind::Check && m == BM_SETCHECK) || (s->kind == Kind::Slider && m == TBM_SETPOS) ||
        (s->kind == Kind::Meter && m == PBM_SETPOS) || (s->kind == Kind::Combo && m == CB_SETCURSEL))
        animate(h, *s);
    if (m == WM_SHOWWINDOW && !w) {
        s->hover = false;
        s->hover_motion.to = 0;
        s->finish();
        KillTimer(h, motion_timer);
        s->timer_running = false;
    }
    return result;
}
} // namespace
bool motion_enabled() { return allow_motion; }
void refresh_motion_settings() {
    BOOL client = TRUE, effects = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &client, 0);
    SystemParametersInfoW(SPI_GETUIEFFECTS, 0, &effects, 0);
    allow_motion = client && effects;
}
void settle(HWND h) {
    DWORD_PTR p = 0;
    if (GetWindowSubclass(h, subclass, 1, &p)) {
        auto *s = reinterpret_cast<State *>(p);
        s->finish();
        KillTimer(h, motion_timer);
        s->timer_running = false;
        InvalidateRect(h, nullptr, FALSE);
    }
}
void style(HWND h, Kind kind, Icon shape, COLORREF accent) {
    SetWindowSubclass(h, subclass, 1, reinterpret_cast<DWORD_PTR>(new State{kind, shape, accent}));
}
void selected(HWND h, bool value) {
    DWORD_PTR p = 0;
    if (GetWindowSubclass(h, subclass, 1, &p)) {
        auto *s = reinterpret_cast<State *>(p);
        if (s->selected != value) {
            s->selected = value;
            animate(h, *s);
        }
    }
}
void text_color(HWND h, COLORREF value) {
    SetPropW(h, L"PMTextColor", reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(value) + 1));
}
void rounded(HDC dc, RECT rect, float radius, COLORREF fill, COLORREF stroke) {
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return;
    Gdiplus::Graphics g(dc);
    setup(g);
    Gdiplus::GraphicsPath p;
    path(p, rect, radius);
    Gdiplus::SolidBrush b(color(fill));
    g.FillPath(&b, &p);
    if (stroke != CLR_INVALID) {
        Gdiplus::Pen pen(color(stroke), 1);
        g.DrawPath(&pen, &p);
    }
}
void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF value) {
    Gdiplus::Graphics g(dc);
    Gdiplus::Pen pen(color(value), 1);
    g.DrawLine(&pen, x1, y1, x2, y2);
}
void text(HDC dc, std::wstring_view value, RECT rect, HFONT font, COLORREF value_color, UINT flags) {
    auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, value_color);
    DrawTextW(dc, value.data(), static_cast<int>(value.size()), &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}
void icon(HDC dc, Icon shape, RECT rect, COLORREF value) {
    Gdiplus::Graphics g(dc);
    setup(g);
    g.TranslateTransform(float(rect.left), float(rect.top));
    g.ScaleTransform(float(rect.right - rect.left) / 24, float(rect.bottom - rect.top) / 24);
    Gdiplus::Pen p(color(value), 1.7f);
    p.SetStartCap(Gdiplus::LineCapRound);
    p.SetEndCap(Gdiplus::LineCapRound);
    p.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush b(color(value));
    auto ln = [&](float a, float c, float d, float e) { g.DrawLine(&p, a, c, d, e); };
    switch (shape) {
    case Icon::Wave:
        ln(3, 9, 3, 15);
        ln(7.5f, 5, 7.5f, 19);
        ln(12, 2, 12, 22);
        ln(16.5f, 6, 16.5f, 18);
        ln(21, 9, 21, 15);
        break;
    case Icon::Mix:
        for (int i = 0; i < 3; ++i) {
            float y = 6.0f + i * 6;
            ln(3, y, 21, y);
            float x = i == 1 ? 15.0f : 8.0f;
            g.FillEllipse(&b, x - 2, y - 2, 4.0f, 4.0f);
        }
        break;
    case Icon::Mic:
        g.DrawArc(&p, 8.0f, 2.0f, 8.0f, 12.0f, 0, 360);
        g.DrawArc(&p, 5.0f, 5.0f, 14.0f, 13.0f, 0, 180);
        ln(5, 10, 5, 12);
        ln(19, 10, 19, 12);
        ln(12, 18, 12, 22);
        ln(8, 22, 16, 22);
        break;
    case Icon::Music:
        ln(10, 17, 10, 5);
        ln(10, 5, 20, 3);
        ln(20, 3, 20, 15);
        g.DrawEllipse(&p, 4.0f, 15.0f, 6.0f, 5.0f);
        g.DrawEllipse(&p, 14.0f, 13.0f, 6.0f, 5.0f);
        break;
    case Icon::Play: {
        Gdiplus::PointF points[] = {{8, 4}, {20, 12}, {8, 20}};
        g.FillPolygon(&b, points, 3);
        break;
    }
    case Icon::Stop:
        g.FillRectangle(&b, 6.0f, 6.0f, 12.0f, 12.0f);
        break;
    case Icon::Record:
        g.FillEllipse(&b, 5.0f, 5.0f, 14.0f, 14.0f);
        break;
    case Icon::Refresh:
        g.DrawArc(&p, 4.0f, 4.0f, 16.0f, 16.0f, 35, 290);
        ln(20, 3, 20, 9);
        ln(20, 9, 14, 9);
        break;
    case Icon::Folder:
        ln(3, 6, 10, 6);
        ln(10, 6, 12, 9);
        ln(12, 9, 21, 9);
        ln(21, 9, 21, 20);
        ln(21, 20, 3, 20);
        ln(3, 20, 3, 6);
        break;
    case Icon::Book:
        ln(12, 5, 12, 21);
        ln(3, 4, 3, 18);
        ln(21, 4, 21, 18);
        ln(3, 4, 9, 4);
        ln(9, 4, 12, 6);
        ln(12, 6, 15, 4);
        ln(15, 4, 21, 4);
        ln(3, 18, 9, 18);
        ln(9, 18, 12, 21);
        ln(12, 21, 15, 18);
        ln(15, 18, 21, 18);
        break;
    case Icon::Headphones:
        g.DrawArc(&p, 3.0f, 3.0f, 18.0f, 18.0f, 180, 180);
        g.DrawRectangle(&p, 3.0f, 11.0f, 4.0f, 9.0f);
        g.DrawRectangle(&p, 17.0f, 11.0f, 4.0f, 9.0f);
        break;
    case Icon::Cable:
        ln(6, 3, 6, 9);
        ln(11, 3, 11, 9);
        ln(3, 9, 14, 9);
        g.DrawArc(&p, 3.0f, 4.0f, 11.0f, 12.0f, 0, 180);
        ln(8.5f, 16, 8.5f, 21);
        ln(8.5f, 21, 20, 21);
        break;
    default:
        break;
    }
}
void draw_combo_item(const DRAWITEMSTRUCT &item, HFONT font) {
    if (item.itemID == UINT(-1))
        return;
    const bool highlight = (item.itemState & ODS_SELECTED) != 0;
    HBRUSH b = CreateSolidBrush(highlight ? teal_soft : white);
    FillRect(item.hDC, &item.rcItem, b);
    DeleteObject(b);
    const auto count = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
    if (count == CB_ERR)
        return;
    std::wstring value(size_t(count) + 1, L'\0');
    SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(value.data()));
    value.resize(size_t(count));
    RECT r = item.rcItem;
    r.left += 12;
    r.right -= 10;
    text(item.hDC, value, r, font, highlight ? teal_text : ink,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
} // namespace partymic::ui
