#include "ui_font.h"

#include <algorithm>
#include <cmath>

namespace esp_pika {
namespace ui_gfx {
namespace {

constexpr uint8_t kFont5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00}, // '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50}, // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '\''
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // '('
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // ')'
    {0x08, 0x2A, 0x1C, 0x2A, 0x08}, // '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00}, // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02}, // '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46}, // '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03}, // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00}, // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ';'
    {0x00, 0x08, 0x14, 0x22, 0x41}, // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // '='
    {0x41, 0x22, 0x14, 0x08, 0x00}, // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06}, // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 'E'
    {0x7F, 0x09, 0x09, 0x01, 0x01}, // 'F'
    {0x3E, 0x41, 0x41, 0x51, 0x32}, // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 'L'
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, // 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 'V'
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 'X'
    {0x03, 0x04, 0x78, 0x04, 0x03}, // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 'Z'
    {0x00, 0x00, 0x7F, 0x41, 0x41}, // '['
    {0x02, 0x04, 0x08, 0x10, 0x20}, // '\\'
    {0x41, 0x41, 0x7F, 0x00, 0x00}, // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04}, // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40}, // '_'
    {0x00, 0x01, 0x02, 0x04, 0x00}, // '`'
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 'a'
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20}, // 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // 'f'
    {0x08, 0x14, 0x54, 0x54, 0x3C}, // 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // 'j'
    {0x00, 0x7F, 0x10, 0x28, 0x44}, // 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20}, // 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00}, // '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // '|'
    {0x00, 0x41, 0x36, 0x08, 0x00}, // '}'
    {0x08, 0x04, 0x08, 0x10, 0x08}, // '~'
};

bool InsideRoundedRect(int px, int py, int x, int y, int w, int h, int r) {
    if (px < x || py < y || px >= x + w || py >= y + h) {
        return false;
    }
    if (r <= 0) {
        return true;
    }
    const int cx = std::clamp(px, x + r, x + w - 1 - r);
    const int cy = std::clamp(py, y + r, y + h - 1 - r);
    const int dx = px - cx;
    const int dy = py - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

} // namespace

uint16_t BlendRgb565(uint16_t bg, uint16_t fg, uint8_t alpha) {
    if (alpha == 0) {
        return bg;
    }
    if (alpha == 255) {
        return fg;
    }
    const uint32_t a = alpha;
    const uint32_t inv = 255U - a;
    const uint32_t br = (bg >> 11) & 0x1FU;
    const uint32_t bgc = (bg >> 5) & 0x3FU;
    const uint32_t bb = bg & 0x1FU;

    const uint32_t fr = (fg >> 11) & 0x1FU;
    const uint32_t fgc = (fg >> 5) & 0x3FU;
    const uint32_t fb = fg & 0x1FU;

    const uint32_t r = (br * inv + fr * a + 127U) / 255U;
    const uint32_t g = (bgc * inv + fgc * a + 127U) / 255U;
    const uint32_t b = (bb * inv + fb * a + 127U) / 255U;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void PutPixelBlend(uint16_t* fb, int fb_w, int fb_h, int x, int y, uint16_t color, uint8_t alpha) {
    if (fb == nullptr || alpha == 0 || x < 0 || y < 0 || x >= fb_w || y >= fb_h) {
        return;
    }
    const size_t idx = static_cast<size_t>(y) * fb_w + x;
    fb[idx] = (alpha == 255) ? color : BlendRgb565(fb[idx], color, alpha);
}

void FillRect(uint16_t* fb, int fb_w, int fb_h, int x, int y, int w, int h, uint16_t color, uint8_t alpha) {
    if (fb == nullptr || alpha == 0 || w <= 0 || h <= 0) {
        return;
    }
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(fb_w, x + w);
    const int y1 = std::min(fb_h, y + h);
    for (int py = y0; py < y1; ++py) {
        uint16_t* row = fb + static_cast<size_t>(py) * fb_w;
        if (alpha == 255) {
            std::fill(row + x0, row + x1, color);
        } else {
            for (int px = x0; px < x1; ++px) {
                row[px] = BlendRgb565(row[px], color, alpha);
            }
        }
    }
}

void FillRoundedRect(uint16_t* fb,
                     int fb_w,
                     int fb_h,
                     int x,
                     int y,
                     int w,
                     int h,
                     int radius,
                     uint16_t color,
                     uint8_t alpha) {
    if (fb == nullptr || alpha == 0 || w <= 0 || h <= 0) {
        return;
    }
    const int r = std::min({radius, w / 2, h / 2});
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(fb_w, x + w);
    const int y1 = std::min(fb_h, y + h);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            if (InsideRoundedRect(px, py, x, y, w, h, r)) {
                PutPixelBlend(fb, fb_w, fb_h, px, py, color, alpha);
            }
        }
    }
}

void StrokeRoundedRect(uint16_t* fb,
                       int fb_w,
                       int fb_h,
                       int x,
                       int y,
                       int w,
                       int h,
                       int radius,
                       uint16_t color,
                       uint8_t alpha) {
    if (fb == nullptr || alpha == 0 || w <= 2 || h <= 2) {
        return;
    }
    const int r = std::min({radius, w / 2, h / 2});
    const int inner_r = std::max(0, r - 1);
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(fb_w, x + w);
    const int y1 = std::min(fb_h, y + h);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            if (InsideRoundedRect(px, py, x, y, w, h, r) &&
                !InsideRoundedRect(px, py, x + 1, y + 1, w - 2, h - 2, inner_r)) {
                PutPixelBlend(fb, fb_w, fb_h, px, py, color, alpha);
            }
        }
    }
}

void FillCircle(uint16_t* fb, int fb_w, int fb_h, int cx, int cy, int r, uint16_t color, uint8_t alpha) {
    if (fb == nullptr || alpha == 0 || r <= 0) {
        return;
    }
    const int r2 = r * r;
    const int y0 = std::max(0, cy - r);
    const int y1 = std::min(fb_h - 1, cy + r);
    const int x0 = std::max(0, cx - r);
    const int x1 = std::min(fb_w - 1, cx + r);
    for (int py = y0; py <= y1; ++py) {
        const int dy = py - cy;
        for (int px = x0; px <= x1; ++px) {
            const int dx = px - cx;
            if (dx * dx + dy * dy <= r2) {
                PutPixelBlend(fb, fb_w, fb_h, px, py, color, alpha);
            }
        }
    }
}

std::string NormalizeAsciiText(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        const uint8_t c = static_cast<uint8_t>(raw[i]);
        if (c < 0x80) {
            if (c == '\r' || c == '\n' || c == '\t') {
                if (out.empty() || out.back() != ' ') {
                    out.push_back(' ');
                }
            } else if (c >= 0x20 && c <= 0x7E) {
                out.push_back(static_cast<char>(c));
            }
            ++i;
            continue;
        }
        size_t seq_len = 1;
        if ((c & 0xE0) == 0xC0) {
            seq_len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            seq_len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            seq_len = 4;
        }
        i = std::min(raw.size(), i + seq_len);
        if (out.empty() || out.back() != ' ') {
            out.push_back(' ');
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::vector<std::string> WrapAsciiText(const std::string& ascii, size_t max_cols, size_t max_lines) {
    std::vector<std::string> lines;
    if (max_cols == 0 || max_lines == 0) {
        return lines;
    }
    size_t pos = 0;
    while (pos < ascii.size() && lines.size() < max_lines) {
        while (pos < ascii.size() && ascii[pos] == ' ') {
            ++pos;
        }
        if (pos >= ascii.size()) {
            break;
        }
        size_t take = std::min(max_cols, ascii.size() - pos);
        if (pos + take < ascii.size() && ascii[pos + take] != ' ') {
            const size_t last_space = ascii.rfind(' ', pos + take);
            if (last_space != std::string::npos && last_space > pos) {
                take = last_space - pos;
            }
        }
        lines.push_back(ascii.substr(pos, take));
        pos += take;
    }
    return lines;
}

void DrawChar5x7(uint16_t* fb,
                 int fb_w,
                 int fb_h,
                 int x,
                 int y,
                 char ch,
                 int scale,
                 uint16_t color,
                 uint8_t alpha) {
    if (fb == nullptr || alpha == 0 || scale <= 0) {
        return;
    }
    if (ch < 0x20 || ch > 0x7E) {
        ch = ' ';
    }
    const uint8_t* glyph = kFont5x7[ch - 0x20];
    for (int col = 0; col < 5; ++col) {
        const uint8_t bits = glyph[col];
        for (int row = 0; row < 7; ++row) {
            if ((bits & (1U << row)) != 0) {
                FillRect(fb, fb_w, fb_h, x + col * scale, y + row * scale, scale, scale, color, alpha);
            }
        }
    }
}

void DrawText(uint16_t* fb,
              int fb_w,
              int fb_h,
              int x,
              int y,
              const std::string& text,
              int scale,
              uint16_t color,
              uint8_t alpha) {
    if (text.empty() || scale <= 0) {
        return;
    }
    const int advance = 6 * scale;
    int cur_x = x;
    for (char ch : text) {
        DrawChar5x7(fb, fb_w, fb_h, cur_x, y, ch, scale, color, alpha);
        cur_x += advance;
    }
}

void DrawCenteredText(uint16_t* fb,
                      int fb_w,
                      int fb_h,
                      int center_x,
                      int y,
                      const std::string& text,
                      int scale,
                      uint16_t color,
                      uint8_t alpha) {
    if (text.empty() || scale <= 0) {
        return;
    }
    const int advance = 6 * scale;
    const int total_w = static_cast<int>(text.size()) * advance - scale;
    DrawText(fb, fb_w, fb_h, center_x - total_w / 2, y, text, scale, color, alpha);
}

void DrawWifiIcon(uint16_t* fb, int fb_w, int fb_h, int x, int y, bool online, uint16_t color) {
    const int cx = x + 9;
    const int cy = y + 12;
    for (int dy = -12; dy <= 0; ++dy) {
        for (int dx = -10; dx <= 10; ++dx) {
            if (std::abs(dx) > -dy + 1) {
                continue;
            }
            const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            const bool arc = (d >= 9.0f && d <= 10.7f) || (d >= 5.2f && d <= 6.8f) || (d <= 1.8f);
            if (arc) {
                PutPixelBlend(fb, fb_w, fb_h, cx + dx, cy + dy, color, online ? 235 : 85);
            }
        }
    }
    if (!online) {
        for (int i = 0; i < 12; ++i) {
            PutPixelBlend(fb, fb_w, fb_h, x + 3 + i, y + 12 - i, 0xF800, 220);
        }
    }
}

void DrawLinkIcon(uint16_t* fb, int fb_w, int fb_h, int x, int y, bool online, uint16_t color) {
    const uint8_t a = online ? 230 : 75;
    StrokeRoundedRect(fb, fb_w, fb_h, x, y + 3, 9, 8, 3, color, a);
    StrokeRoundedRect(fb, fb_w, fb_h, x + 7, y + 3, 9, 8, 3, color, a);
    FillRect(fb, fb_w, fb_h, x + 5, y + 6, 6, 2, color, a);
}

void DrawBatteryIcon(uint16_t* fb,
                     int fb_w,
                     int fb_h,
                     int x,
                     int y,
                     int percent,
                     bool charging,
                     uint16_t frame_color,
                     uint16_t fill_color) {
    const int pct = std::clamp(percent, 0, 100);
    StrokeRoundedRect(fb, fb_w, fb_h, x, y + 1, 24, 12, 2, frame_color, 210);
    FillRect(fb, fb_w, fb_h, x + 24, y + 4, 2, 6, frame_color, 210);
    const int fill_w = (pct * 18 + 50) / 100;
    if (fill_w > 0) {
        FillRect(fb, fb_w, fb_h, x + 3, y + 4, fill_w, 6, fill_color, 235);
    }
    if (charging) {
        const uint16_t bolt = 0xFFE0; // warm bright yellow
        FillRect(fb, fb_w, fb_h, x + 11, y + 2, 3, 4, bolt, 255);
        FillRect(fb, fb_w, fb_h, x + 9, y + 5, 6, 2, bolt, 255);
        FillRect(fb, fb_w, fb_h, x + 11, y + 6, 3, 4, bolt, 255);
    }
}

void DrawVolumeIcon(uint16_t* fb, int fb_w, int fb_h, int x, int y, uint16_t color) {
    FillRect(fb, fb_w, fb_h, x, y + 5, 4, 6, color, 235);
    for (int i = 0; i < 5; ++i) {
        FillRect(fb, fb_w, fb_h, x + 4 + i, y + 4 - i, 1, 8 + i * 2, color, 235);
    }
    FillRect(fb, fb_w, fb_h, x + 11, y + 4, 2, 8, color, 210);
    FillRect(fb, fb_w, fb_h, x + 14, y + 2, 2, 12, color, 175);
}

void DrawBrightnessIcon(uint16_t* fb, int fb_w, int fb_h, int x, int y, uint16_t color) {
    FillCircle(fb, fb_w, fb_h, x + 8, y + 8, 3, color, 240);
    FillRect(fb, fb_w, fb_h, x + 7, y + 1, 2, 3, color, 220);
    FillRect(fb, fb_w, fb_h, x + 7, y + 12, 2, 3, color, 220);
    FillRect(fb, fb_w, fb_h, x + 1, y + 7, 3, 2, color, 220);
    FillRect(fb, fb_w, fb_h, x + 12, y + 7, 3, 2, color, 220);
}

} // namespace ui_gfx
} // namespace esp_pika
