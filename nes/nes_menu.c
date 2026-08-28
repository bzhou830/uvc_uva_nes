/*
 * nes_menu.c - 上电游戏选择菜单的帧缓冲渲染。
 *
 * 全部在 256x240 RGB565 缓冲上用 16x16 点阵字体画文字。颜色直接是 RGB565
 * 值（R 高 5 位 / G 中 6 位 / B 低 5 位）。菜单帧缓冲复用 g_nes_rom 的前
 * 120KB（菜单态 ROM 区空闲），不额外占内存。
 */
#include <stdint.h>
#include <string.h>

#include "nes_menu.h"
#include "nes_menu_font.h"

/* ---- 调色板（RGB565） ---- */
#define MENU_BG      0x0000  /* 黑 */
#define MENU_TITLE    0xFFE0  /* 黄 */
#define MENU_ITEM     0xFFFF  /* 白 */
#define MENU_SEL_BG   0x001F  /* 蓝 */
#define MENU_SEL_FG   0xFFFF  /* 白 */
#define MENU_MARK     0x07E0  /* 绿（选择箭头 ">"） */
#define MENU_HINT     0x7BEF  /* 灰 */

#define GLYPH_W  16
#define GLYPH_H  16
#define ADVANCE  18      /* 每个字形水平步进（16 + 2 间隙） */
#define ROW_H    22      /* 列表每行高度 */

/* 画一个 16x16 点阵字形到 (x,y)。bit==1 用 fg，否则用 bg。 */
static void draw_glyph(uint16_t *fb, int fb_w, int x, int y,
                        const uint8_t *bits, uint16_t fg, uint16_t bg)
{
    if (!bits) return;  /* 缺字：留空 */
    for (int gy = 0; gy < GLYPH_H; gy++) {
        uint8_t b0 = bits[gy * 2 + 0];   /* 左半 8 列 (MSB-first) */
        uint8_t b1 = bits[gy * 2 + 1];   /* 右半 8 列 */
        for (int gx = 0; gx < GLYPH_W; gx++) {
            int on = (gx < 8) ? (b0 & (0x80 >> gx))
                              : (b1 & (0x80 >> (gx - 8)));
            int px = x + gx, py = y + gy;
            if (px < 0 || py < 0 || px >= 256 || py >= 240) continue;
            fb[py * fb_w + px] = on ? fg : bg;
        }
    }
}

/* UTF-8 解码：返回码点，并前进 *p。非法序列回退到 0xFFFD。 */
static int utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned char c = *s;
    if (c == 0) { return -1; }
    if (c < 0x80) { *p = (const char *)(s + 1); return c; }
    if ((c & 0xE0) == 0xC0 && s[1]) {
        int cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        *p = (const char *)(s + 2); return cp;
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        int cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p = (const char *)(s + 3); return cp;
    }
    *p = (const char *)(s + 1); return 0xFFFD;
}

/* 从 (x,y) 起画 UTF-8 字符串，返回绘制到的下一个 x（便于需要时用）。 */
static void draw_text(uint16_t *fb, int fb_w, int x, int y,
                      const char *s, uint16_t fg, uint16_t bg)
{
    if (!s) return;
    const char *p = s;
    int cp;
    while ((cp = utf8_next(&p)) >= 0) {
        const uint8_t *bits = menu_glyph_bits((uint32_t)cp);
        draw_glyph(fb, fb_w, x, y, bits, fg, bg);
        x += ADVANCE;
    }
}

/* 填充矩形（用于选中行高亮背景）。 */
static void fill_rect(uint16_t *fb, int fb_w, int x0, int y0, int w, int h, uint16_t c)
{
    for (int yy = y0; yy < y0 + h; yy++) {
        if (yy < 0 || yy >= 240) continue;
        for (int xx = x0; xx < x0 + w; xx++) {
            if (xx < 0 || xx >= 256) continue;
            fb[yy * fb_w + xx] = c;
        }
    }
}

void nes_menu_render(uint16_t *fb, int fb_w, int fb_h,
                     const nes_menu_item_t *items, int count, int sel,
                     const char *title, const char *hint)
{
    /* 背景清黑 */
    for (int i = 0; i < fb_w * fb_h; i++) fb[i] = MENU_BG;

    /* 标题 */
    draw_text(fb, fb_w, 8, 8, title, MENU_TITLE, MENU_BG);

    /* 列表 */
    int y = 40;
    for (int i = 0; i < count; i++) {
        if (i == sel) {
            fill_rect(fb, fb_w, 0, y, fb_w, ROW_H, MENU_SEL_BG);
            draw_text(fb, fb_w, 4,  y + 3, ">",   MENU_MARK,  MENU_SEL_BG);
            draw_text(fb, fb_w, 22, y + 3, items[i].name, MENU_SEL_FG, MENU_SEL_BG);
        } else {
            draw_text(fb, fb_w, 22, y + 3, items[i].name, MENU_ITEM, MENU_BG);
        }
        y += ROW_H;
    }

    /* 底部提示 */
    if (hint) {
        draw_text(fb, fb_w, 4, fb_h - 20, hint, MENU_HINT, MENU_BG);
    }
}
