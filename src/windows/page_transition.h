#pragma once
#include <windows.h>

namespace partymic::ui {
// A short-lived content overlay. Native controls are laid out once and remain
// interactive underneath; animation frames only composite two local bitmaps.
class PageTransition {
  public:
    ~PageTransition();
    bool prepare(HWND parent, const RECT &bounds);
    bool start();
    void tick();
    void stop();
    bool active() const { return active_; }

  private:
    struct Surface {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        int width = 0, height = 0;
        ~Surface();
        bool create(int w, int h);
        void clear();
    } before_, after_, frame_;
    HWND parent_ = nullptr, overlay_ = nullptr;
    RECT bounds_{};
    ULONGLONG started_ = 0;
    ULONGLONG last_paint_ = 0, max_gap_ = 0;
    unsigned frames_ = 0;
    bool active_ = false;
    bool preparing_ = false;
    void capture(HDC dc);
    void compose(HDC dc);
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
};
} // namespace partymic::ui
