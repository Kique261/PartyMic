#pragma once
#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>
#include <string_view>

namespace partymic::ui {
inline constexpr COLORREF white = RGB(255, 255, 255), sidebar = RGB(245, 245, 247),
                          subtle = RGB(239, 239, 242);
inline constexpr COLORREF border = RGB(228, 228, 234), strong = RGB(207, 207, 214), ink = RGB(24, 24, 27),
                          muted = RGB(101, 101, 110);
inline constexpr COLORREF orange = RGB(249, 115, 22), orange_hover = RGB(234, 98, 10),
                          orange_soft = RGB(255, 241, 230);
inline constexpr COLORREF teal = RGB(13, 148, 136), teal_text = RGB(6, 114, 107),
                          teal_soft = RGB(231, 246, 244);
enum class Kind { Neutral, Primary, Nav, Check, Slider, Combo, Meter };
enum class Icon { None, Wave, Mix, Mic, Book, Music, Refresh, Play, Stop, Record, Folder, Headphones, Cable };
bool motion_enabled();
void refresh_motion_settings();
void settle(HWND window);
void style(HWND window, Kind kind, Icon icon = Icon::None, COLORREF accent = teal);
void selected(HWND window, bool value);
void text_color(HWND window, COLORREF value);
void rounded(HDC dc, RECT rect, float radius, COLORREF fill, COLORREF stroke = CLR_INVALID);
void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color);
void icon(HDC dc, Icon shape, RECT rect, COLORREF color);
void text(HDC dc, std::wstring_view value, RECT rect, HFONT font, COLORREF color,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE);
void draw_combo_item(const DRAWITEMSTRUCT &item, HFONT font);
} // namespace partymic::ui
