/*
 * nes_menu.h - 上电游戏选择菜单（渲染到 256x240 RGB565 帧缓冲）。
 *
 * 菜单由 nes_bridge_run() 在 UVC 视频流上显示，HID（方向键 + A/Start）
 * 选择，选中后加载对应 Flash 槽里的 ROM 运行。文字用 nes_menu_font.h
 * 里构建期生成的中文 16x16 点阵字体绘制。
 */
#ifndef NES_MENU_H
#define NES_MENU_H

#include <stdint.h>

/* 一个菜单项：UTF-8 显示名 + 对应 ROM 的 Flash 物理偏移（仅用于探测/调试）。 */
typedef struct {
    const char *name;   /* UTF-8 字符串（菜单用到的字必须已包含在字体里） */
    uint32_t    offset; /* ROM 槽 Flash 偏移 */
} nes_menu_item_t;

/*
 * 把游戏列表渲染到 RGB565 帧缓冲 fb（尺寸 fb_w x fb_h）。
 *   items / count : 可选项（已烧录的 ROM）；count==0 时只画标题与提示。
 *   sel           : 当前高亮项索引（0..count-1），-1 表示不高亮。
 *   title / hint  : 顶部标题与底部提示（UTF-8）。
 */
void nes_menu_render(uint16_t *fb, int fb_w, int fb_h,
                     const nes_menu_item_t *items, int count, int sel,
                     const char *title, const char *hint);

#endif /* NES_MENU_H */
