#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace esp_pika {
namespace ui_gfx {

/// Alpha-blends foreground RGB565 onto existing pixel in framebuffer (`alpha` in [0, 255]).
uint16_t BlendRgb565(uint16_t bg, uint16_t fg, uint8_t alpha);

/// Writes an alpha-blended pixel safely within [0, fb_w) x [0, fb_h).
void PutPixelBlend(uint16_t* fb,
                   int fb_w,
                   int fb_h,
                   int x,
                   int y,
                   uint16_t color,
                   uint8_t alpha = 255);

void FillRect(uint16_t* fb,
              int fb_w,
              int fb_h,
              int x,
              int y,
              int w,
              int h,
              uint16_t color,
              uint8_t alpha = 255);

void FillRoundedRect(uint16_t* fb,
                     int fb_w,
                     int fb_h,
                     int x,
                     int y,
                     int w,
                     int h,
                     int radius,
                     uint16_t color,
                     uint8_t alpha = 255);

void StrokeRoundedRect(uint16_t* fb,
                       int fb_w,
                       int fb_h,
                       int x,
                       int y,
                       int w,
                       int h,
                       int radius,
                       uint16_t color,
                       uint8_t alpha = 255);

void FillCircle(uint16_t* fb,
                int fb_w,
                int fb_h,
                int cx,
                int cy,
                int r,
                uint16_t color,
                uint8_t alpha = 255);

/// Sanitizes UTF-8 input into printable ASCII lines (prevents garbled multi-byte glyphs).
std::string NormalizeAsciiText(const std::string& raw);
std::vector<std::string> WrapAsciiText(const std::string& ascii, size_t max_cols, size_t max_lines);

void DrawChar5x7(uint16_t* fb,
                 int fb_w,
                 int fb_h,
                 int x,
                 int y,
                 char ch,
                 int scale,
                 uint16_t color,
                 uint8_t alpha = 255);

void DrawText(uint16_t* fb,
              int fb_w,
              int fb_h,
              int x,
              int y,
              const std::string& text,
              int scale,
              uint16_t color,
              uint8_t alpha = 255);

void DrawCenteredText(uint16_t* fb,
                      int fb_w,
                      int fb_h,
                      int center_x,
                      int y,
                      const std::string& text,
                      int scale,
                      uint16_t color,
                      uint8_t alpha = 255);

/// Crisp micro HUD icons (designed at ~18x14 / 26x14 for minimal OLED top bar).
void DrawWifiIcon(uint16_t* fb, int fb_w, int fb_h, int x, int y, bool online, uint16_t color);
void DrawLinkIcon(uint16_t* fb, int fb_w, int fb_h, int x, int y, bool online, uint16_t color);
void DrawBatteryIcon(uint16_t* fb,
                     int fb_w,
                     int fb_h,
                     int x,
                     int y,
                     int percent,
                     bool charging,
                     uint16_t frame_color,
                     uint16_t fill_color);
void DrawVolumeIcon(uint16_t* fb, int fb_w, int fb_h, int x, int y, uint16_t color);
void DrawBrightnessIcon(uint16_t* fb, int fb_w, int fb_h, int x, int y, uint16_t color);

} // namespace ui_gfx
} // namespace esp_pika
