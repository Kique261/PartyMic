#include "windows/page_transition.h"
#include <algorithm>

namespace partymic::ui {
PageTransition::Surface::~Surface() { clear(); }
void PageTransition::Surface::clear() {
    if (dc && previous)
        SelectObject(dc, previous);
    if (bitmap)
        DeleteObject(bitmap);
    if (dc)
        DeleteDC(dc);
    dc = nullptr;
    bitmap = nullptr;
    previous = nullptr;
    width = height = 0;
}
bool PageTransition::Surface::create(int w, int h) {
    if (dc && bitmap && width == w && height == h)
        return true;
    clear();
    if (w <= 0 || h <= 0)
        return false;
    dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) {
        clear();
        return false;
    }
    previous = SelectObject(dc, bitmap);
    width = w;
    height = h;
    return true;
}
PageTransition::~PageTransition() { stop(); }
void PageTransition::capture(HDC dc) {
    const int saved = SaveDC(dc);
    SetViewportOrgEx(dc, -bounds_.left, -bounds_.top, nullptr);
    IntersectClipRect(dc, bounds_.left, bounds_.top, bounds_.right, bounds_.bottom);
    SendMessageW(parent_, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    // Print only this application's immediate visible children. Viewports and
    // clipping keep a control from drawing outside its normal window rectangle.
    for (HWND child = GetWindow(parent_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (child == overlay_ || !IsWindowVisible(child))
            continue;
        RECT r{};
        GetWindowRect(child, &r);
        MapWindowPoints(nullptr, parent_, reinterpret_cast<POINT *>(&r), 2);
        RECT intersection{};
        if (!IntersectRect(&intersection, &r, &bounds_))
            continue;
        const int control_saved = SaveDC(dc);
        IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
        SetViewportOrgEx(dc, r.left - bounds_.left, r.top - bounds_.top, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                     PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        RestoreDC(dc, control_saved);
    }
    RestoreDC(dc, saved);
}
void PageTransition::compose(HDC dc) {
    const int w = before_.width, h = before_.height;
    BitBlt(dc, 0, 0, w, h, before_.dc, 0, 0, SRCCOPY);
    if (preparing_)
        return;
    const double t = std::clamp(double(GetTickCount64() - started_) / 200, 0.0, 1.0);
    const double eased = 1 - (1 - t) * (1 - t) * (1 - t);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, BYTE(eased * 255 + 0.5), 0};
    AlphaBlend(dc, 0, 0, w, h, after_.dc, 0, 0, w, h, blend);
}
bool PageTransition::prepare(HWND parent, const RECT &bounds) {
    Surface current;
    const bool interrupted = active_;
    if (interrupted) {
        if (!current.create(before_.width, before_.height)) {
            stop();
            return false;
        }
        compose(current.dc);
    }
    // Keep an existing shield alive during rapid navigation. Destroying it
    // here would briefly expose the previous transition's live destination.
    active_ = false;
    parent_ = parent;
    bounds_ = bounds;
    const int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    if (!before_.create(w, h) || !after_.create(w, h) || !frame_.create(w, h)) {
        stop();
        return false;
    }
    if (interrupted && current.width == w && current.height == h)
        BitBlt(before_.dc, 0, 0, w, h, current.dc, 0, 0, SRCCOPY);
    else
        capture(before_.dc);
    if (!overlay_) {
        WNDCLASSW type{};
        type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"PartyMicPageTransition";
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            stop();
            return false;
        }
        overlay_ =
            CreateWindowExW(WS_EX_NOACTIVATE, type.lpszClassName, L"", WS_CHILD, bounds_.left, bounds_.top,
                            before_.width, before_.height, parent_, nullptr, type.hInstance, this);
        if (!overlay_) {
            stop();
            return false;
        }
    }
    // Show the old pixels BEFORE any destination controls move or repaint.
    // While staging, redraw requests must keep presenting this same snapshot.
    preparing_ = active_ = true;
    SetWindowPos(overlay_, HWND_TOP, bounds_.left, bounds_.top, before_.width, before_.height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(overlay_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    return true;
}
bool PageTransition::start() {
    if (!parent_ || !before_.dc || !overlay_)
        return false;
    capture(after_.dc);
    started_ = GetTickCount64();
    last_paint_ = max_gap_ = frames_ = 0;
    preparing_ = false;
    active_ = true;
    RedrawWindow(overlay_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    return true;
}
void PageTransition::tick() {
    if (!active_ || preparing_)
        return;
    if (GetTickCount64() - started_ >= 200) {
        stop();
        return;
    }
    RedrawWindow(overlay_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}
void PageTransition::stop() {
    if (active_ && !preparing_ && IsWindow(parent_)) {
        // Local diagnostics for frame cadence; no hooks or background sampling.
        SetPropW(parent_, L"PartyMicTransitionFrames", reinterpret_cast<HANDLE>(ULONG_PTR(frames_)));
        SetPropW(parent_, L"PartyMicTransitionMaxGapMs", reinterpret_cast<HANDLE>(ULONG_PTR(max_gap_)));
        SetPropW(parent_, L"PartyMicTransitionDurationMs",
                 reinterpret_cast<HANDLE>(ULONG_PTR(GetTickCount64() - started_)));
    }
    active_ = false;
    preparing_ = false;
    if (overlay_ && IsWindow(overlay_))
        DestroyWindow(overlay_);
    overlay_ = nullptr;
    before_.clear();
    after_.clear();
    frame_.clear();
}
LRESULT CALLBACK PageTransition::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto *self = reinterpret_cast<PageTransition *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<PageTransition *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND)
        return 1;
    if (self && self->active_ && (message == WM_PAINT || message == WM_PRINTCLIENT || message == WM_PRINT)) {
        if (message == WM_PAINT) {
            const auto now = GetTickCount64();
            if (self->frames_)
                self->max_gap_ = std::max(self->max_gap_, now - self->last_paint_);
            self->last_paint_ = now;
            ++self->frames_;
        }
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? BeginPaint(window, &ps) : reinterpret_cast<HDC>(wparam);
        self->compose(self->frame_.dc);
        BitBlt(dc, 0, 0, self->frame_.width, self->frame_.height, self->frame_.dc, 0, 0, SRCCOPY);
        if (message == WM_PAINT)
            EndPaint(window, &ps);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace partymic::ui
