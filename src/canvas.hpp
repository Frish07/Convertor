#pragma once

#include <windows.h>
#include <string_view>

namespace ui {
struct Box {
    int x, y, w, h;
    bool contains(POINT point) const {
        return point.x >= x && point.x < x + w && point.y >= y && point.y < y + h;
    }
};

inline constexpr COLORREF background = RGB(19, 23, 24);
inline constexpr COLORREF panel = RGB(25, 30, 31);
inline constexpr COLORREF elevated = RGB(33, 38, 39);
inline constexpr COLORREF border = RGB(48, 54, 54);
inline constexpr COLORREF text = RGB(243, 242, 239);
inline constexpr COLORREF muted = RGB(175, 181, 179);
inline constexpr COLORREF faint = RGB(134, 145, 144);
inline constexpr COLORREF red = RGB(153, 44, 51);
inline constexpr COLORREF redHover = RGB(179, 53, 61);
inline constexpr COLORREF blush = RGB(245, 159, 164);
inline constexpr COLORREF wine = RGB(48, 29, 32);
inline constexpr COLORREF green = RGB(130, 205, 163);

enum class Icon { video, image, audio, file, add, arrow, chevron, close, folder, swap, search };
enum class Font { small, normal, strong, brand, headline, compactHeadline };

class Canvas {
public:
    Canvas(HDC target, UINT dpi, int width, int height);
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    int px(int value) const { return MulDiv(value, dpi_, 96); }
    void rect(Box box, COLORREF fill);
    void round(Box box, COLORREF fill, COLORREF outline, int radius = 6);
    void circle(Box box, COLORREF fill, COLORREF outline);
    void gradient(Box box, COLORREF left, COLORREF right, bool horizontal);
    void line(int x1, int y1, int x2, int y2, COLORREF color, int width = 1);
    void label(Box box, std::wstring_view value, COLORREF color, Font font = Font::normal,
               UINT alignment = DT_LEFT);
    void icon(Icon shape, int x, int y, int size, COLORREF color);
    void logo(HICON image, Box box);

private:
    RECT bounds(Box box) const;
    HDC target_, memory_;
    HBITMAP bitmap_;
    HGDIOBJ previous_;
    HFONT fonts_[6]{};
    UINT dpi_;
    int width_, height_;
};
}
