#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

extern uint8_t lcd_font[240][10];

class LcdRenderer {
public:
    static constexpr int kWidth = 820;
    static constexpr int kHeight = 100;

    LcdRenderer() : pixels_(kWidth * kHeight, kBackColor) {
        reset();
    }

    void write(uint32_t address, uint8_t data) {
        if (address == 0) {
            if ((data & 0xe0) == 0x20) {
                lcdDL_ = (data & 0x10) != 0;
                lcdN_ = (data & 0x8) != 0;
                lcdF_ = (data & 0x4) != 0;
            } else if ((data & 0xf8) == 0x8) {
                lcdD_ = (data & 0x4) != 0;
                lcdC_ = (data & 0x2) != 0;
                lcdB_ = (data & 0x1) != 0;
            } else if (data == 0x01) {
                lcdDdRam_ = 0;
                lcdId_ = 1;
                std::memset(lcdData_, 0x20, sizeof(lcdData_));
            } else if (data == 0x02) {
                lcdDdRam_ = 0;
            } else if ((data & 0xfc) == 0x04) {
                lcdId_ = (data & 0x2) != 0;
                lcdS_ = (data & 0x1) != 0;
            } else if ((data & 0xc0) == 0x40) {
                lcdCgRam_ = (data & 0x3f);
                lcdRamMode_ = 0;
            } else if ((data & 0x80) == 0x80) {
                lcdDdRam_ = (data & 0x7f);
                lcdRamMode_ = 1;
            }
            return;
        }

        if (!lcdRamMode_) {
            lcdCg_[lcdCgRam_] = data & 0x1f;
            lcdCgRam_ += lcdId_ ? 1 : -1;
            lcdCgRam_ &= 0x3f;
            return;
        }

        if (lcdN_) {
            int off = (lcdDdRam_ & 0x40) ? 40 : 0;
            if ((lcdDdRam_ & 0x3f) < 40) {
                lcdData_[(lcdDdRam_ & 0x3f) + off] = data;
            }
        } else if (lcdDdRam_ < 80) {
            lcdData_[lcdDdRam_] = data;
        }

        lcdDdRam_ += lcdId_ ? 1 : -1;
        lcdDdRam_ &= 0x7f;
    }

    void render() {
        std::fill(pixels_.begin(), pixels_.end(), kBackColor);

        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 24; ++col) {
                uint8_t ch = lcdData_[row * 40 + col];
                renderGlyph(4 + row * 50, 4 + col * 34, ch);
            }
        }

        const int col = lcdDdRam_ % 0x40;
        const int row = lcdDdRam_ / 0x40;
        if (lcdC_ && row < 2 && col < 24) {
            renderGlyph(4 + row * 50, 4 + col * 34, static_cast<uint8_t>('_'), true);
        }
    }

    const std::vector<uint32_t>& pixels() const {
        return pixels_;
    }

    void reset() {
        lcdDL_ = 0;
        lcdN_ = 0;
        lcdF_ = 0;
        lcdD_ = 0;
        lcdC_ = 0;
        lcdB_ = 0;
        lcdId_ = 1;
        lcdS_ = 0;
        lcdDdRam_ = 0;
        lcdAc_ = 0;
        lcdCgRam_ = 0;
        lcdRamMode_ = 0;
        std::memset(lcdData_, 0x20, sizeof(lcdData_));
        std::memset(lcdCg_, 0, sizeof(lcdCg_));
    }

private:
    void renderGlyph(int32_t x, int32_t y, uint8_t ch, bool overlay = false, int scale = 5) {
        const uint8_t* glyph = (ch >= 16) ? &lcd_font[ch - 16][0] : &lcdCg_[(ch & 7) * 8];

        for (int i = 0; i < 7; ++i) {
            for (int j = 0; j < 5; ++j) {
                const uint32_t color = (glyph[i] & (1 << (4 - j))) ? kFgColor : kBgColor;
                const int xx = x + i * (scale + 1);
                const int yy = y + j * (scale + 1);
                for (int ii = 0; ii < scale; ++ii) {
                    for (int jj = 0; jj < scale; ++jj) {
                        const int pxRow = xx + ii;
                        const int pxCol = yy + jj;
                        if (pxRow < 0 || pxRow >= kHeight || pxCol < 0 || pxCol >= kWidth) {
                            continue;
                        }
                        uint32_t& px = pixels_[pxRow * kWidth + pxCol];
                        if (overlay) {
                            px &= color;
                        } else {
                            px = color;
                        }
                    }
                }
            }
        }
    }

    static constexpr uint32_t kBackColor = 0xFF03BE51;
    static constexpr uint32_t kFgColor = 0x000000;
    static constexpr uint32_t kBgColor = 0x78B500;

    bool lcdDL_ {};
    bool lcdN_ {};
    bool lcdF_ {};
    bool lcdD_ {};
    bool lcdC_ {};
    bool lcdB_ {};
    bool lcdId_ {true};
    bool lcdS_ {};

    uint32_t lcdDdRam_ {};
    uint32_t lcdAc_ {};
    uint32_t lcdCgRam_ {};
    uint32_t lcdRamMode_ {};

    uint8_t lcdData_[80] {};
    uint8_t lcdCg_[64] {};

    std::vector<uint32_t> pixels_;
};
