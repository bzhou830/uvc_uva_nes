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

/* 帧缓冲尺寸（用于裁剪）。由 nes_menu_render 设置，draw_glyph/fill_rect 共用，
 * 避免在底层绘制函数里硬编码 256/240 而在别的分辨率下越界。 */
static int s_fb_w = 256;
static int s_fb_h = 240;

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
            if (px < 0 || py < 0 || px >= s_fb_w || py >= s_fb_h) continue;
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
        if (yy < 0 || yy >= s_fb_h) continue;
        for (int xx = x0; xx < x0 + w; xx++) {
            if (xx < 0 || xx >= s_fb_w) continue;
            fb[yy * fb_w + xx] = c;
        }
    }
}

/* 列表区几何：起始 y、底部提示行预留高度。 */
#define LIST_TOP     40
#define HINT_RESERVE 20      /* 底部提示行占的高度，列表不得进入 */

void nes_menu_render(uint16_t *fb, int fb_w, int fb_h,
                     const nes_menu_item_t *items, int count, int sel,
                     const char *title, const char *hint)
{
    /* 供底层绘制函数裁剪用。 */
    s_fb_w = fb_w;
    s_fb_h = fb_h;

    /* 背景清黑 */
    for (int i = 0; i < fb_w * fb_h; i++) fb[i] = MENU_BG;

    /* 标题 */
    draw_text(fb, fb_w, 8, 8, title, MENU_TITLE, MENU_BG);

    /* ---- 列表（带滚动窗口）----
     * 可视行数由屏幕高度决定，保证列表绝不进入底部提示行区域；
     * 选中项始终保持在窗口内，超出部分通过滚动显示。
     * 滚动时用右侧上/下三角指示还有更多条目。 */
    int list_h = fb_h - HINT_RESERVE - LIST_TOP;
    int visible = list_h / ROW_H;
    if (visible < 1) visible = 1;

    int first = 0;
    if (count > visible) {
        /* 让 sel 落在 [first, first+visible-1] 内。 */
        if (sel < first) {
            first = sel;
        } else if (sel >= first + visible) {
            first = sel - visible + 1;
        }
        if (first < 0) first = 0;
        if (first > count - visible) first = count - visible;
    }

    int shown = (count - first < visible) ? (count - first) : visible;

    for (int i = 0; i < shown; i++) {
        int idx = first + i;
        int y = LIST_TOP + i * ROW_H;
        if (idx == sel) {
            fill_rect(fb, fb_w, 0, y, fb_w, ROW_H, MENU_SEL_BG);
            draw_text(fb, fb_w, 4,  y + 3, ">",   MENU_MARK,  MENU_SEL_BG);
            draw_text(fb, fb_w, 22, y + 3, items[idx].name, MENU_SEL_FG, MENU_SEL_BG);
        } else {
            draw_text(fb, fb_w, 22, y + 3, items[idx].name, MENU_ITEM, MENU_BG);
        }
    }

    /* 滚动指示：上三角（还有上文）/ 下三角（还有下文），画在右侧边缘。
     * 用 3 行像素拼出实心三角，位置取列表区首行 / 末行的垂直中心。 */
    if (count > visible) {
        if (first > 0) {
            int cy = LIST_TOP + ROW_H / 2 - 1;
            int cx = fb_w - 12;
            for (int r = 0; r < 3; r++) {
                fill_rect(fb, fb_w, cx + r, cy - r, 5 - 2 * r, 1, MENU_HINT);
            }
        }
        if (first + visible < count) {
            int cy = LIST_TOP + (visible - 1) * ROW_H + ROW_H / 2 - 1;
            int cx = fb_w - 12;
            for (int r = 0; r < 3; r++) {
                fill_rect(fb, fb_w, cx + (2 - r), cy - r, 1 + 2 * r, 1, MENU_HINT);
            }
        }
    }

    /* 底部提示 */
    if (hint) {
        draw_text(fb, fb_w, 4, fb_h - HINT_RESERVE, hint, MENU_HINT, MENU_BG);
    }
}
