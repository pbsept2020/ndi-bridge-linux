/**
 * tc_font.h — Professional 7-segment timecode display renderer
 *
 * Pixel-perfect rendering of broadcast-style timecode display
 * inspired by Tentacle Sync E, Ambient ACN Lockit, Denecke TS-3.
 *
 * Features:
 *   - Beveled/angular segment endpoints (trapezoid shape)
 *   - Ghost segments (inactive segments visible in dark gray)
 *   - Orange glow/halo effect on active segments
 *   - Drop-frame semicolon separator
 *   - Heartbeat indicators (alternating dots)
 *   - Mini 5x7 bitmap font for framerate label
 *   - All dimensions scale proportionally with resolution
 *
 * No external font dependencies — everything is drawn pixel by pixel.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace tc {

// ── Colors ──────────────────────────────────────────────────────────

struct Color {
    uint8_t r, g, b, a;
};

static constexpr Color COL_PANEL_BG    = {0x0A, 0x0A, 0x0A, 0xFF};  // deep black panel
static constexpr Color COL_PANEL_BORDER= {0x2A, 0x2A, 0x2A, 0xFF};  // subtle gray border
static constexpr Color COL_SEG_OFF     = {0x1A, 0x1A, 0x1A, 0xFF};  // ghost segment
static constexpr Color COL_SEG_ON      = {0xFF, 0x8C, 0x00, 0xFF};  // orange active
static constexpr Color COL_GLOW        = {0xFF, 0x8C, 0x00, 0x40};  // orange 25% for glow
static constexpr Color COL_SHADOW      = {0x00, 0x00, 0x00, 0x60};  // drop shadow
static constexpr Color COL_LABEL       = {0x88, 0x88, 0x88, 0xFF};  // framerate label gray
static constexpr Color COL_INDICATOR_ON  = {0xFF, 0x8C, 0x00, 0xFF};
static constexpr Color COL_INDICATOR_OFF = {0x1A, 0x1A, 0x1A, 0xFF};

// ── 7-segment digit map ─────────────────────────────────────────────
//
//  Segment layout:
//     ─ 0 ─
//    |       |
//    5       1
//    |       |
//     ─ 6 ─
//    |       |
//    4       2
//    |       |
//     ─ 3 ─
//

static constexpr uint8_t SEGS[10] = {
    0b0111111,  // 0: segments 0,1,2,3,4,5
    0b0000110,  // 1: segments 1,2
    0b1011011,  // 2: segments 0,1,3,4,6
    0b1001111,  // 3: segments 0,1,2,3,6
    0b1100110,  // 4: segments 1,2,5,6
    0b1101101,  // 5: segments 0,2,3,5,6
    0b1111101,  // 6: segments 0,2,3,4,5,6
    0b0000111,  // 7: segments 0,1,2
    0b1111111,  // 8: all segments
    0b1101111,  // 9: segments 0,1,2,3,5,6
};

// ── Mini 5x7 bitmap font for framerate labels ───────────────────────
// Each character is 5 columns x 7 rows, stored as 7 bytes (1 byte per row, LSB = left)

static const uint8_t MINI_FONT[][7] = {
    // '0' = index 0
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    // '1'
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    // '2'
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    // '3'
    {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E},
    // '4'
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    // '5'
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    // '6'
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    // '7'
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    // '8'
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    // '9'
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    // ' ' = index 10
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // '.' = index 11
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
    // 'D' = index 12
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},
    // 'F' = index 13
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    // 'N' = index 14
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
    // 'f' = index 15
    {0x06, 0x08, 0x08, 0x1C, 0x08, 0x08, 0x08},
    // 'p' = index 16
    {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10},
    // 's' = index 17
    {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E},
    // 'i' = index 18  (added for "fps" label - not needed, use f,p,s)
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},
};

// Map ASCII to mini font index (-1 = not available)
static inline int miniFontIndex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    switch (c) {
        case ' ': return 10;
        case '.': return 11;
        case 'D': return 12;
        case 'F': return 13;
        case 'N': return 14;
        case 'f': return 15;
        case 'p': return 16;
        case 's': return 17;
        case 'i': return 18;
        default:  return 10; // space for unknown
    }
}

// ── Scaling helper ──────────────────────────────────────────────────

struct TCLayout {
    // All dimensions in pixels, scaled from 1080p reference
    float scale;

    int digitW;          // digit width
    int digitH;          // digit height
    int segThick;        // segment thickness
    int segGap;          // gap between adjacent segments
    int digitSpacing;    // space between digits within a pair
    int groupSpacing;    // space around colon separators
    int colonW;          // colon dot size
    int padH;            // horizontal padding inside panel
    int padV;            // vertical padding inside panel
    int panelW;          // total panel width
    int panelH;          // total panel height
    int panelX;          // panel top-left X
    int panelY;          // panel top-left Y
    int borderW;         // border width
    int cornerR;         // corner radius
    int indicatorR;      // heartbeat indicator radius
    int indicatorGap;    // gap between indicator and last digit
    int glowR;           // glow radius in pixels
    int shadowOfsX;      // shadow offset X
    int shadowOfsY;      // shadow offset Y
    int labelFontH;      // mini font character height
    int labelFontW;      // mini font character width
    int labelY;          // label Y position (below panel)

    void compute(int imgW, int imgH) {
        scale = static_cast<float>(imgH) / 1080.0f;

        digitW       = static_cast<int>(36.0f * scale);
        digitH       = static_cast<int>(60.0f * scale);
        segThick     = std::max(2, static_cast<int>(5.0f * scale));
        segGap       = std::max(1, static_cast<int>(1.5f * scale));
        digitSpacing = static_cast<int>(6.0f * scale);
        groupSpacing = static_cast<int>(14.0f * scale);
        colonW       = std::max(2, static_cast<int>(4.0f * scale));
        padH         = static_cast<int>(12.0f * scale);
        padV         = static_cast<int>(15.0f * scale);
        borderW      = std::max(1, static_cast<int>(2.0f * scale));
        cornerR      = std::max(2, static_cast<int>(6.0f * scale));
        indicatorR   = std::max(3, static_cast<int>(5.0f * scale));
        indicatorGap = static_cast<int>(8.0f * scale);
        glowR        = std::max(1, static_cast<int>(2.0f * scale));
        shadowOfsX   = std::max(1, static_cast<int>(3.0f * scale));
        shadowOfsY   = std::max(1, static_cast<int>(4.0f * scale));
        labelFontH   = std::max(5, static_cast<int>(7.0f * scale));
        labelFontW   = std::max(4, static_cast<int>(5.0f * scale));

        // Panel width: HH:MM:SS:FF = 8 digits + 3 separators + 2 indicators
        // Within each pair: 2 digits + 1 digitSpacing
        int pairW = digitW * 2 + digitSpacing;
        int sepW = groupSpacing * 2 + colonW;  // space + colon + space
        int indicatorsW = indicatorGap + indicatorR * 2;

        panelW = padH * 2 + pairW * 4 + sepW * 3 + indicatorsW;
        panelH = padV * 2 + digitH;

        panelX = (imgW - panelW) / 2;
        panelY = static_cast<int>(imgH * 0.85f) - panelH / 2;

        // Clamp to image bounds
        if (panelY + panelH + shadowOfsY >= imgH)
            panelY = imgH - panelH - shadowOfsY - 4;

        labelY = panelY + panelH + static_cast<int>(4.0f * scale);
    }
};

// ── Pixel blending ──────────────────────────────────────────────────

static inline void blendPixel(uint8_t* dst, Color c) {
    // dst is BGRA
    if (c.a == 255) {
        dst[0] = c.b;
        dst[1] = c.g;
        dst[2] = c.r;
        dst[3] = 255;
    } else if (c.a > 0) {
        float a = c.a / 255.0f;
        float inv = 1.0f - a;
        dst[0] = static_cast<uint8_t>(c.b * a + dst[0] * inv);
        dst[1] = static_cast<uint8_t>(c.g * a + dst[1] * inv);
        dst[2] = static_cast<uint8_t>(c.r * a + dst[2] * inv);
        dst[3] = 255;
    }
}

static inline void setPixel(uint8_t* img, int stride, int imgW, int imgH,
                              int px, int py, Color c) {
    if (px < 0 || px >= imgW || py < 0 || py >= imgH) return;
    blendPixel(img + py * stride + px * 4, c);
}

// ── Filled rectangle with optional rounded corners ──────────────────

static void fillRect(uint8_t* img, int stride, int imgW, int imgH,
                     int rx, int ry, int rw, int rh, Color c, int radius = 0) {
    for (int y = ry; y < ry + rh; y++) {
        for (int x = rx; x < rx + rw; x++) {
            if (radius > 0) {
                // Check corners
                int dx = 0, dy = 0;
                if (x < rx + radius && y < ry + radius) {
                    dx = rx + radius - x; dy = ry + radius - y;
                } else if (x >= rx + rw - radius && y < ry + radius) {
                    dx = x - (rx + rw - radius - 1); dy = ry + radius - y;
                } else if (x < rx + radius && y >= ry + rh - radius) {
                    dx = rx + radius - x; dy = y - (ry + rh - radius - 1);
                } else if (x >= rx + rw - radius && y >= ry + rh - radius) {
                    dx = x - (rx + rw - radius - 1); dy = y - (ry + rh - radius - 1);
                }
                if (dx * dx + dy * dy > radius * radius) continue;
            }
            setPixel(img, stride, imgW, imgH, x, y, c);
        }
    }
}

// ── Filled circle ───────────────────────────────────────────────────

static void fillCircle(uint8_t* img, int stride, int imgW, int imgH,
                       int cx, int cy, int r, Color c) {
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) {
                setPixel(img, stride, imgW, imgH, x, y, c);
            }
        }
    }
}

// ── Border rectangle (outline only) ─────────────────────────────────

static void borderRect(uint8_t* img, int stride, int imgW, int imgH,
                       int rx, int ry, int rw, int rh, Color c,
                       int thick, int radius) {
    // Draw by filling the border area: outer rect minus inner rect
    for (int y = ry; y < ry + rh; y++) {
        for (int x = rx; x < rx + rw; x++) {
            bool inBorder = (x < rx + thick || x >= rx + rw - thick ||
                             y < ry + thick || y >= ry + rh - thick);
            if (!inBorder) continue;

            // Check rounded corners on outer boundary
            if (radius > 0) {
                int dx = 0, dy = 0;
                if (x < rx + radius && y < ry + radius) {
                    dx = rx + radius - x; dy = ry + radius - y;
                } else if (x >= rx + rw - radius && y < ry + radius) {
                    dx = x - (rx + rw - radius - 1); dy = ry + radius - y;
                } else if (x < rx + radius && y >= ry + rh - radius) {
                    dx = rx + radius - x; dy = y - (ry + rh - radius - 1);
                } else if (x >= rx + rw - radius && y >= ry + rh - radius) {
                    dx = x - (rx + rw - radius - 1); dy = y - (ry + rh - radius - 1);
                }
                if (dx * dx + dy * dy > radius * radius) continue;
            }
            setPixel(img, stride, imgW, imgH, x, y, c);
        }
    }
}

// ── Beveled 7-segment drawing ───────────────────────────────────────
//
// Each segment is drawn as a hexagon/trapezoid shape rather than a plain
// rectangle. The bevel angle clips the endpoints at ~45 degrees.
//
// Horizontal segment (e.g., top):
//     ___________
//    /           \       <- beveled ends
//    \___________/
//
// Vertical segment (e.g., right side):
//    /\
//    ||
//    ||
//    \/                  <- beveled top and bottom
//

static void drawHSegment(uint8_t* img, int stride, int imgW, int imgH,
                          int x0, int y0, int len, int thick, Color c) {
    // Horizontal segment with beveled ends
    int bevel = thick / 2;
    int halfT = thick / 2;

    for (int row = -halfT; row <= halfT; row++) {
        int py = y0 + row;
        // How far from center (0 = center, halfT = edge)
        int dist = std::abs(row);
        // Bevel shrinks the segment at top/bottom edges
        int xStart = x0 + dist;
        int xEnd   = x0 + len - dist;

        for (int px = xStart; px < xEnd; px++) {
            setPixel(img, stride, imgW, imgH, px, py, c);
        }
    }
}

static void drawVSegment(uint8_t* img, int stride, int imgW, int imgH,
                          int x0, int y0, int len, int thick, Color c) {
    // Vertical segment with beveled ends
    int halfT = thick / 2;

    for (int col = -halfT; col <= halfT; col++) {
        int px = x0 + col;
        int dist = std::abs(col);
        int yStart = y0 + dist;
        int yEnd   = y0 + len - dist;

        for (int py = yStart; py < yEnd; py++) {
            setPixel(img, stride, imgW, imgH, px, py, c);
        }
    }
}

// Draw glow around a horizontal segment
static void drawHSegmentGlow(uint8_t* img, int stride, int imgW, int imgH,
                              int x0, int y0, int len, int thick, Color glowC, int glowR) {
    int halfT = thick / 2;
    // Expand the bounding box by glowR
    for (int row = -halfT - glowR; row <= halfT + glowR; row++) {
        int py = y0 + row;
        int dist = std::abs(row);
        // Only draw in glow region (outside the segment body)
        if (dist <= halfT) continue;  // inside segment, skip
        int glowDist = dist - halfT;
        if (glowDist > glowR) continue;
        float alpha = 1.0f - static_cast<float>(glowDist) / (glowR + 1);
        Color gc = glowC;
        gc.a = static_cast<uint8_t>(gc.a * alpha);

        int xStart = x0 + halfT;  // approximate
        int xEnd   = x0 + len - halfT;
        for (int px = xStart; px < xEnd; px++) {
            setPixel(img, stride, imgW, imgH, px, py, gc);
        }
    }
}

// Draw glow around a vertical segment
static void drawVSegmentGlow(uint8_t* img, int stride, int imgW, int imgH,
                              int x0, int y0, int len, int thick, Color glowC, int glowR) {
    int halfT = thick / 2;
    for (int col = -halfT - glowR; col <= halfT + glowR; col++) {
        int px = x0 + col;
        int dist = std::abs(col);
        if (dist <= halfT) continue;
        int glowDist = dist - halfT;
        if (glowDist > glowR) continue;
        float alpha = 1.0f - static_cast<float>(glowDist) / (glowR + 1);
        Color gc = glowC;
        gc.a = static_cast<uint8_t>(gc.a * alpha);

        int yStart = y0 + halfT;
        int yEnd   = y0 + len - halfT;
        for (int py = yStart; py < yEnd; py++) {
            setPixel(img, stride, imgW, imgH, px, py, gc);
        }
    }
}

// ── Draw a single 7-segment digit ───────────────────────────────────

static void drawDigit(uint8_t* img, int stride, int imgW, int imgH,
                      int x, int y, int digit, const TCLayout& L) {
    if (digit < 0 || digit > 9) return;

    uint8_t segs = SEGS[digit];
    int w = L.digitW;
    int h = L.digitH;
    int t = L.segThick;
    int g = L.segGap;
    int halfH = h / 2;

    // Segment positions (center coordinates for each segment)
    // Seg 0: top horizontal
    // Seg 1: top-right vertical
    // Seg 2: bottom-right vertical
    // Seg 3: bottom horizontal
    // Seg 4: bottom-left vertical
    // Seg 5: top-left vertical
    // Seg 6: middle horizontal

    struct SegDef { bool horiz; int cx, cy, len; };
    SegDef defs[7] = {
        {true,  x + w / 2,       y + t / 2,                   w - 2 * g},       // 0: top
        {false, x + w - t / 2,   y + t + g + (halfH - t - g) / 2, halfH - t - g}, // 1: top-right
        {false, x + w - t / 2,   y + halfH + g + (halfH - t - g) / 2, halfH - t - g}, // 2: bot-right
        {true,  x + w / 2,       y + h - t / 2,               w - 2 * g},       // 3: bottom
        {false, x + t / 2,       y + halfH + g + (halfH - t - g) / 2, halfH - t - g}, // 4: bot-left
        {false, x + t / 2,       y + t + g + (halfH - t - g) / 2, halfH - t - g}, // 5: top-left
        {true,  x + w / 2,       y + halfH,                   w - 2 * g},       // 6: middle
    };

    // Draw all 7 segments — ghost (off) first, then active on top
    for (int s = 0; s < 7; s++) {
        auto& d = defs[s];
        Color c = COL_SEG_OFF;  // ghost

        if (d.horiz) {
            drawHSegment(img, stride, imgW, imgH,
                         d.cx - d.len / 2, d.cy, d.len, t, c);
        } else {
            drawVSegment(img, stride, imgW, imgH,
                         d.cx, d.cy - d.len / 2 + t / 2, d.len, t, c);
        }
    }

    // Now draw active segments with glow
    for (int s = 0; s < 7; s++) {
        if (!(segs & (1 << s))) continue;
        auto& d = defs[s];

        // Glow first (behind the segment)
        if (d.horiz) {
            drawHSegmentGlow(img, stride, imgW, imgH,
                             d.cx - d.len / 2, d.cy, d.len, t, COL_GLOW, L.glowR);
            drawHSegment(img, stride, imgW, imgH,
                         d.cx - d.len / 2, d.cy, d.len, t, COL_SEG_ON);
        } else {
            drawVSegmentGlow(img, stride, imgW, imgH,
                             d.cx, d.cy - d.len / 2 + t / 2, d.len, t, COL_GLOW, L.glowR);
            drawVSegment(img, stride, imgW, imgH,
                         d.cx, d.cy - d.len / 2 + t / 2, d.len, t, COL_SEG_ON);
        }
    }
}

// ── Draw colon or semicolon separator ───────────────────────────────

static void drawSeparator(uint8_t* img, int stride, int imgW, int imgH,
                          int cx, int y, int digitH, int dotSize,
                          bool dropFrame, Color c) {
    // Two dots stacked vertically
    int topDotY = y + digitH / 3;
    int botDotY = y + digitH * 2 / 3;
    int botDotX = cx;

    // For drop-frame semicolon, offset bottom dot slightly right
    if (dropFrame) {
        botDotX += dotSize;
    }

    fillCircle(img, stride, imgW, imgH, cx, topDotY, dotSize, c);
    fillCircle(img, stride, imgW, imgH, botDotX, botDotY, dotSize, c);
}

// ── Draw heartbeat indicators ───────────────────────────────────────

static void drawHeartbeat(uint8_t* img, int stride, int imgW, int imgH,
                          int cx, int y, int digitH, int radius,
                          bool frameEven) {
    int topY = y + digitH / 3;
    int botY = y + digitH * 2 / 3;

    // Alternating: even frame = top ON, bottom OFF; odd = inverse
    // Always draw ghost first
    fillCircle(img, stride, imgW, imgH, cx, topY, radius, COL_INDICATOR_OFF);
    fillCircle(img, stride, imgW, imgH, cx, botY, radius, COL_INDICATOR_OFF);

    // Active dot
    if (frameEven) {
        fillCircle(img, stride, imgW, imgH, cx, topY, radius, COL_INDICATOR_ON);
    } else {
        fillCircle(img, stride, imgW, imgH, cx, botY, radius, COL_INDICATOR_ON);
    }
}

// ── Draw mini font character ────────────────────────────────────────

static void drawMiniChar(uint8_t* img, int stride, int imgW, int imgH,
                         int x, int y, char c, float charScale, Color col) {
    int idx = miniFontIndex(c);
    if (idx < 0) return;

    int cw = static_cast<int>(5 * charScale);
    int ch = static_cast<int>(7 * charScale);

    for (int row = 0; row < ch; row++) {
        int srcRow = row * 7 / ch;
        if (srcRow >= 7) srcRow = 6;
        uint8_t bits = MINI_FONT[idx][srcRow];
        for (int col_i = 0; col_i < cw; col_i++) {
            int srcCol = col_i * 5 / cw;
            if (srcCol >= 5) srcCol = 4;
            if (bits & (1 << srcCol)) {
                setPixel(img, stride, imgW, imgH, x + col_i, y + row, col);
            }
        }
    }
}

// ── Draw mini font string ───────────────────────────────────────────

static void drawMiniString(uint8_t* img, int stride, int imgW, int imgH,
                           int x, int y, const char* str, float charScale, Color col) {
    int cw = static_cast<int>(5 * charScale) + std::max(1, static_cast<int>(charScale));
    int cx = x;
    for (const char* p = str; *p; p++) {
        drawMiniChar(img, stride, imgW, imgH, cx, y, *p, charScale, col);
        cx += cw;
    }
}

// Width of a mini font string in pixels
static int miniStringWidth(const char* str, float charScale) {
    int cw = static_cast<int>(5 * charScale) + std::max(1, static_cast<int>(charScale));
    int len = 0;
    for (const char* p = str; *p; p++) len++;
    return len > 0 ? len * cw - std::max(1, static_cast<int>(charScale)) : 0;
}

// ── Main timecode display rendering ─────────────────────────────────

/**
 * Draw the full timecode display panel on an image.
 *
 * @param img       BGRA pixel buffer
 * @param stride    bytes per row
 * @param imgW      image width
 * @param imgH      image height
 * @param hh,mm,ss,ff  timecode values
 * @param dropFrame true if drop-frame timecode (semicolon separator before FF)
 * @param frameNum  frame counter for heartbeat alternation
 * @param fpsLabel  string like "25 fps" or "29.97 DF" shown below panel
 */
static void drawTimecodeDisplay(uint8_t* img, int stride, int imgW, int imgH,
                                 int hh, int mm, int ss, int ff,
                                 bool dropFrame, int frameNum,
                                 const char* fpsLabel) {
    TCLayout L;
    L.compute(imgW, imgH);

    // ── Drop shadow ──
    fillRect(img, stride, imgW, imgH,
             L.panelX + L.shadowOfsX, L.panelY + L.shadowOfsY,
             L.panelW, L.panelH, COL_SHADOW, L.cornerR);

    // ── Panel background ──
    fillRect(img, stride, imgW, imgH,
             L.panelX, L.panelY, L.panelW, L.panelH,
             COL_PANEL_BG, L.cornerR);

    // ── Panel border ──
    borderRect(img, stride, imgW, imgH,
               L.panelX, L.panelY, L.panelW, L.panelH,
               COL_PANEL_BORDER, L.borderW, L.cornerR);

    // ── Digit positions ──
    int digitsY = L.panelY + L.padV;
    int cx = L.panelX + L.padH;

    // Digits: HH : MM : SS : FF [indicators]
    int digits[8] = {
        hh / 10, hh % 10,
        mm / 10, mm % 10,
        ss / 10, ss % 10,
        ff / 10, ff % 10
    };

    for (int pair = 0; pair < 4; pair++) {
        // First digit of pair
        drawDigit(img, stride, imgW, imgH, cx, digitsY, digits[pair * 2], L);
        cx += L.digitW + L.digitSpacing;

        // Second digit of pair
        drawDigit(img, stride, imgW, imgH, cx, digitsY, digits[pair * 2 + 1], L);
        cx += L.digitW;

        // Separator after pairs 0, 1, 2 (not after FF)
        if (pair < 3) {
            cx += L.groupSpacing;
            bool isDropFrameSep = (pair == 2 && dropFrame);  // last separator before FF
            Color sepColor = COL_SEG_ON;
            drawSeparator(img, stride, imgW, imgH,
                          cx + L.colonW / 2, digitsY, L.digitH, L.colonW,
                          isDropFrameSep, sepColor);
            cx += L.colonW + L.groupSpacing;
        }
    }

    // ── Heartbeat indicators ──
    cx += L.indicatorGap;
    drawHeartbeat(img, stride, imgW, imgH,
                  cx + L.indicatorR, digitsY, L.digitH,
                  L.indicatorR, (frameNum % 2) == 0);

    // ── Framerate label ──
    if (fpsLabel && fpsLabel[0]) {
        float labelScale = L.scale;
        int labelW = miniStringWidth(fpsLabel, labelScale);
        int labelX = L.panelX + (L.panelW - labelW) / 2;
        drawMiniString(img, stride, imgW, imgH,
                       labelX, L.labelY, fpsLabel, labelScale, COL_LABEL);
    }
}

} // namespace tc
