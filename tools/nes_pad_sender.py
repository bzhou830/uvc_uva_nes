#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
nes_pad_sender.py - 把电脑上的手柄/键盘按键转发到 BL616 UVC+UAC+NES 板，
                    通过新加的 HID OUT 端点（接口 4，端点 0x03）喂 NES 按钮位。

硬件侧：usb_composite.c 里 iface4 是一个 HID 设备，报告描述符为
        "1 字节 Output 报告，Report ID=1，vendor usage page 0xFF00"。
        主机每次写 [report_id=1][nes_pad_byte] 即为一次按键快照。

NES 按钮位（与 infones_port.c 的 pad_state 完全对应）：
    A=b0(0x01)  B=b1(0x02)  Select=b2(0x04)  Start=b3(0x08)
    Up=b4(0x10) Down=b5(0x20) Left=b6(0x40) Right=b7(0x80)

依赖：
    pip install hid pygame pynput
  （pygame 读手柄；pynput 读键盘并支持长按/松开/多键同按，无需聚焦窗口；
   未装 pynput 时 --keyboard 自动回退 msvcrt，仅点按、长按不可靠）

键盘映射（--keyboard）：
    w=↑  a=←  s=↓  d=→      j=A  k=B      Enter=Select  Space=Start

用法示例：
    python tools/nes_pad_sender.py                 # 自动找 VID/PID=ABCD/1234 的 HID，用手柄
    python tools/nes_pad_sender.py --keyboard      # 键盘 w/a/s/d=↑←↓→ j/k=A/B Enter=Sel Space=Sta
    python tools/nes_pad_sender.py --list          # 列出所有 HID 设备，找对的 VID/PID
    python tools/nes_pad_sender.py --vid 1234 --pid 5678   # 自定义 VID/PID
"""
import os
import sys

# Windows: Python 3.8+ 默认不再把 exe 所在目录纳入 ctypes 的 DLL 搜索路径，
# 会导致 cython-hidapi 找不到放在 venv/Scripts 的 hidapi.dll。显式加回。
if sys.platform == "win32":
    try:
        os.add_dll_directory(os.path.dirname(sys.executable))
    except (OSError, ValueError):
        pass

import argparse
import time

# ---------- NES 按钮位定义（不要改，必须和固件一致） ----------
BIT_A      = 0x01
BIT_B      = 0x02
BIT_SELECT = 0x04
BIT_START  = 0x08
BIT_UP     = 0x10
BIT_DOWN   = 0x20
BIT_LEFT   = 0x40
BIT_RIGHT  = 0x80

DEFAULT_VID = 0xABCD
DEFAULT_PID = 0x1234
REPORT_ID   = 1


def build_pad(up, down, left, right, a, b, select, start):
    pad = 0
    if up:    pad |= BIT_UP
    if down:  pad |= BIT_DOWN
    if left:  pad |= BIT_LEFT
    if right: pad |= BIT_RIGHT
    if a:     pad |= BIT_A
    if b:     pad |= BIT_B
    if select: pad |= BIT_SELECT
    if start:  pad |= BIT_START
    return pad


def list_devices():
    import hid
    print("=== HID 设备列表 ===")
    for d in hid.enumerate():
        print("  %04X:%04X  %-32s  path=%s  usage=%04X:%04X" % (
            d['vendor_id'], d['product_id'],
            (d.get('product_string') or ''), d['path'],
            d.get('usage_page', 0), d.get('usage', 0)))
    print("(找 BL6x8_UVC_UAC_DEMO 或 VID/PID=%04X/%04X)" % (DEFAULT_VID, DEFAULT_PID))


def send_loop_hid(dev, get_pad):
    """通用发送循环：dev 为已打开的 hid.Device，get_pad() 返回当前 NES 位。"""
    last = None
    print("已连接 HID 设备，开始转发（Ctrl+C 退出）...")
    try:
        while True:
            pad = get_pad()
            if pad != last:
                # 报告格式：[report_id=1][nes_pad_byte]
                buf = bytes([REPORT_ID, pad])
                n = dev.write(buf)
                if n < 0:
                    print("write 错误:", n)
                    break
                last = pad
            time.sleep(0.004)   # ~250 Hz 轮询，足够手柄响应
    except KeyboardInterrupt:
        print("\n退出。")


def main():
    ap = argparse.ArgumentParser(description="Send NES pad state to BL616 over HID OUT")
    ap.add_argument("--vid", type=lambda x: int(x, 0), default=DEFAULT_VID)
    ap.add_argument("--pid", type=lambda x: int(x, 0), default=DEFAULT_PID)
    ap.add_argument("--keyboard", action="store_true", help="用键盘而不是手柄")
    ap.add_argument("--no-pygame", action="store_true", help="不依赖 pygame（仅键盘 msvcrt）")
    ap.add_argument("--list", action="store_true", help="列出 HID 设备后退出")
    args = ap.parse_args()

    if args.list:
        list_devices()
        return

    import hid
    try:
        # cython-hidapi >= 1.0：构造即打开（旧 hid.device().open() 已移除）
        dev = hid.Device(vid=args.vid, pid=args.pid)
    except Exception as e:
        print("无法打开 HID 设备 %04X:%04X: %s" % (args.vid, args.pid, e))
        print("用 --list 查看可用设备，或用 --vid/--pid 指定。")
        return
    dev.nonblocking = True
    print("HID 打开: %04X:%04X  %s" % (args.vid, args.pid, dev.product))

    if args.keyboard or args.no_pygame:
        keyboard_loop(dev)
    else:
        try:
            import pygame
        except ImportError:
            print("未安装 pygame，回退到键盘模式（msvcrt）。")
            keyboard_loop(dev)
            return
        gamepad_loop(dev, pygame)


def keyboard_loop(dev):
    """键盘模式分发：优先 pynput（支持长按/松开/多键同按），否则回退 msvcrt。"""
    try:
        from pynput import keyboard as pnk
        _keyboard_pynput(dev, pnk)
        return
    except ImportError:
        print("提示: 未安装 pynput，回退到 msvcrt（仅点按，长按/松开不可靠）。")
        print("      推荐: pip install pynput  以获得正确的按住/松开。")
        _keyboard_msvcrt(dev)


def _keyboard_pynput(dev, pnk):
    """pynput 全局钩子：按下加入集合、松开移除，每变化即发一次快照。"""
    # 键名 -> NES 位：w/a/s/d=↑←↓→  j/k=A/B  Enter=Select  Space=Start
    keymap = {
        'w': BIT_UP, 'a': BIT_LEFT, 's': BIT_DOWN, 'd': BIT_RIGHT,
        'j': BIT_A, 'k': BIT_B,
        'enter': BIT_SELECT, 'space': BIT_START,
    }
    pressed = set()
    print("键盘模式(pynput): w/a/s/d=↑←↓→  j/k=A/B  Enter=Select  Space=Start  (Ctrl+C 退出)")

    def send():
        pad = 0
        for b in pressed:
            pad |= b
        dev.write(bytes([REPORT_ID, pad]))

    def name_of(key):
        # 字母/符号用 key.char；功能键用 Key.xxx -> 'x'
        if hasattr(key, 'char') and key.char is not None:
            return key.char.lower()
        return str(key).replace('key.', '')

    def on_press(key):
        bit = keymap.get(name_of(key))
        if bit is not None:
            pressed.add(bit)
            send()

    def on_release(key):
        bit = keymap.get(name_of(key))
        if bit is not None:
            pressed.discard(bit)
            send()

    listener = pnk.Listener(on_press=on_press, on_release=on_release)
    listener.start()
    try:
        while True:
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        listener.stop()
        dev.write(bytes([REPORT_ID, 0]))   # 退出时发全松开快照
        print("\n退出。")


def _keyboard_msvcrt(dev):
    """msvcrt 回退：靠 Windows 按键重复维持按住，每帧读缓冲并或运算。
    限制：无真正 keyup，松开靠 ~120ms 内无按键重复推断；不能稳定长按。"""
    import msvcrt
    keymap = {
        b'w': BIT_UP, b's': BIT_DOWN, b'a': BIT_LEFT, b'd': BIT_RIGHT,
        b'W': BIT_UP, b'S': BIT_DOWN, b'A': BIT_LEFT, b'D': BIT_RIGHT,
        b'j': BIT_A, b'k': BIT_B, b'J': BIT_A, b'K': BIT_B,
        b'\r': BIT_SELECT, b' ': BIT_START,   # Enter=Select  Space=Start
        b'\xe0': None,                        # 方向键前缀
    }
    arrow = {b'H': BIT_UP, b'P': BIT_DOWN, b'K': BIT_LEFT, b'M': BIT_RIGHT}
    print("键盘模式(msvcrt): w/a/s/d=↑←↓→  j/k=A/B  Enter=Select  Space=Start  (Ctrl+C 退出)")
    try:
        while True:
            pad = 0
            while msvcrt.kbhit():          # 读尽本帧所有缓冲按键
                ch = msvcrt.getch()
                if ch == b'\x03':          # Ctrl+C
                    return
                if ch == b'\xe0':          # 方向键前缀
                    nxt = msvcrt.getch()
                    bit = arrow.get(nxt)
                    if bit is not None:
                        pad |= bit
                    continue
                bit = keymap.get(ch)
                if bit is not None:
                    pad |= bit
            dev.write(bytes([REPORT_ID, pad]))
            time.sleep(0.12)               # 靠系统按键重复维持"按住"
    except KeyboardInterrupt:
        print("\n退出。")


def gamepad_loop(dev, pygame):
    pygame.init()
    pygame.joystick.init()
    if pygame.joystick.get_count() == 0:
        print("未检测到手柄，请先插上手柄。退出。")
        return
    js = pygame.joystick.Joystick(0)
    js.init()
    print("手柄: %s  按钮数=%d 轴数=%d 帽数=%d" % (
        js.get_name(), js.get_numbuttons(), js.get_numaxes(), js.get_numhats()))
    print("映射: 方向键/左摇杆=方向  A(0)=A  B(1)=B  Back(6)=Select  Start(7)=Start")
    pygame.init()

    def get_pad():
        pygame.event.pump()
        up = down = left = right = a = b = sel = start = False
        # 方向：hat(0) 优先，否则左摇杆轴 0/1
        if js.get_numhats() > 0:
            hx, hy = js.get_hat(0)
            if hx < 0: left = True
            if hx > 0: right = True
            if hy > 0: up = True
            if hy < 0: down = True
        if js.get_numaxes() >= 2:
            if js.get_axis(0) < -0.5: left = True
            if js.get_axis(0) > 0.5: right = True
            if js.get_axis(1) < -0.5: up = True
            if js.get_axis(1) > 0.5: down = True
        # 按钮（XBox 布局常见；不同手柄自行对照 --list/打印调）
        nb = js.get_numbuttons()
        if nb > 0: a = js.get_button(0)
        if nb > 1: b = js.get_button(1)
        if nb > 6: sel = js.get_button(6)   # Back
        if nb > 7: start = js.get_button(7)  # Start
        return build_pad(up, down, left, right, a, b, sel, start)

    send_loop_hid(dev, get_pad)


if __name__ == "__main__":
    main()
