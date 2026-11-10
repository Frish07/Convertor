#include "canvas.hpp"

#include <array>

namespace ui {
Canvas::Canvas(HDC target, UINT dpi, int width, int height)
    : target_(target), memory_(CreateCompatibleDC(target)),
      bitmap_(CreateCompatibleBitmap(target, width, height)), previous_(SelectObject(memory_, bitmap_)),
      dpi_(dpi), width_(width), height_(height) {
    constexpr std::array sizes{12, 14, 14, 24, 35, 29};
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        fonts_[i] = CreateFontW(-px(sizes[i]), 0, 0, 0, i < 2 ? FW_NORMAL : FW_SEMIBOLD,
                                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    SetBkMode(memory_, TRANSPARENT);
}

Canvas::~Canvas() {
    BitBlt(target_, 0, 0, width_, height_, memory_, 0, 0, SRCCOPY);
    SelectObject(memory_, previous_);
    DeleteObject(bitmap_);
    for (auto font : fonts_) DeleteObject(font);
    DeleteDC(memory_);
}

RECT Canvas::bounds(Box box) const {
    return {px(box.x), px(box.y), px(box.x + box.w), px(box.y + box.h)};
}

void Canvas::rect(Box box, COLORREF fill) {
    RECT area = bounds(box);
    auto brush = CreateSolidBrush(fill);
    FillRect(memory_, &area, brush);
    DeleteObject(brush);
}

void Canvas::round(Box box, COLORREF fill, COLORREF outline, int radius) {
    auto area = bounds(box);
    auto brush = CreateSolidBrush(fill);
    auto pen = CreatePen(PS_SOLID, 1, outline);
    auto oldBrush = SelectObject(memory_, brush), oldPen = SelectObject(memory_, pen);
    RoundRect(memory_, area.left, area.top, area.right, area.bottom, px(radius * 2), px(radius * 2));
    SelectObject(memory_, oldBrush);
    SelectObject(memory_, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void Canvas::circle(Box box, COLORREF fill, COLORREF outline) {
    auto area = bounds(box);
    auto brush = CreateSolidBrush(fill);
    auto pen = CreatePen(PS_SOLID, 1, outline);
    auto oldBrush = SelectObject(memory_, brush), oldPen = SelectObject(memory_, pen);
    Ellipse(memory_, area.left, area.top, area.right, area.bottom);
    SelectObject(memory_, oldBrush);
    SelectObject(memory_, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void Canvas::gradient(Box box, COLORREF left, COLORREF right, bool horizontal) {
    auto area = bounds(box);
    auto vertex = [](LONG x, LONG y, COLORREF color) {
        return TRIVERTEX{x, y, static_cast<COLOR16>(GetRValue(color) * 256),
                         static_cast<COLOR16>(GetGValue(color) * 256),
                         static_cast<COLOR16>(GetBValue(color) * 256), 0};
    };
    TRIVERTEX points[]{vertex(area.left, area.top, left), vertex(area.right, area.bottom, right)};
    GRADIENT_RECT corners{0, 1};
    GradientFill(memory_, points, 2, &corners, 1, horizontal ? GRADIENT_FILL_RECT_H : GRADIENT_FILL_RECT_V);
}

void Canvas::line(int x1, int y1, int x2, int y2, COLORREF color, int width) {
    auto pen = CreatePen(PS_SOLID, px(width), color);
    auto oldPen = SelectObject(memory_, pen);
    MoveToEx(memory_, px(x1), px(y1), nullptr);
    LineTo(memory_, px(x2), px(y2));
    SelectObject(memory_, oldPen);
    DeleteObject(pen);
}

void Canvas::label(Box box, std::wstring_view value, COLORREF color, Font font, UINT alignment) {
    if (box.w <= 0 || box.h <= 0 || value.empty()) return;
    auto area = bounds(box);
    auto oldFont = SelectObject(memory_, fonts_[static_cast<int>(font)]);
    SetTextColor(memory_, color);
    DrawTextW(memory_, value.data(), static_cast<int>(value.size()), &area,
              DT_NOPREFIX | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | alignment);
    SelectObject(memory_, oldFont);
}

void Canvas::icon(Icon shape, int x, int y, int size, COLORREF color) {
    auto stroke = [&](int ax, int ay, int bx, int by) {
        line(x + ax * size / 24, y + ay * size / 24, x + bx * size / 24, y + by * size / 24, color, 2);
    };
    switch (shape) {
        case Icon::arrow:
            stroke(4, 12, 20, 12); stroke(14, 6, 20, 12); stroke(20, 12, 14, 18);
            return;
        case Icon::chevron:
            stroke(5, 8, 12, 15); stroke(12, 15, 19, 8);
            return;
        case Icon::close:
            stroke(5, 5, 19, 19); stroke(19, 5, 5, 19);
            return;
        case Icon::swap:
            stroke(4, 8, 19, 8); stroke(15, 4, 19, 8); stroke(19, 8, 15, 12);
            stroke(20, 16, 5, 16); stroke(9, 12, 5, 16); stroke(5, 16, 9, 20);
            return;
        case Icon::folder:
            stroke(3, 6, 10, 6); stroke(10, 6, 12, 9); stroke(12, 9, 21, 9);
            stroke(21, 9, 21, 19); stroke(21, 19, 3, 19); stroke(3, 19, 3, 6);
            return;
        case Icon::search:
            circle({x + size / 8, y + size / 8, size * 5 / 8, size * 5 / 8}, elevated, color);
            stroke(15, 15, 21, 21);
            return;
        default:
            break;
    }
    stroke(5, 2, 14, 2); stroke(14, 2, 20, 8); stroke(20, 8, 20, 22);
    stroke(20, 22, 5, 22); stroke(5, 22, 5, 2); stroke(14, 2, 14, 8); stroke(14, 8, 20, 8);
    if (shape == Icon::add) {
        stroke(8, 15, 17, 15); stroke(12, 11, 12, 19);
    } else if (shape == Icon::image) {
        stroke(7, 19, 10, 15); stroke(10, 15, 12, 17); stroke(12, 17, 15, 12); stroke(15, 12, 19, 19);
    } else if (shape == Icon::video) {
        stroke(10, 11, 15, 15); stroke(15, 15, 10, 19); stroke(10, 19, 10, 11);
    } else if (shape == Icon::audio) {
        stroke(13, 11, 13, 18); stroke(13, 11, 17, 13); stroke(9, 18, 13, 18);
    } else {
        stroke(8, 13, 16, 13); stroke(8, 17, 16, 17);
    }
}

void Canvas::logo(HICON image, Box box) {
    DrawIconEx(memory_, px(box.x), px(box.y), image, px(box.w), px(box.h), 0, nullptr, DI_NORMAL);
}
}
