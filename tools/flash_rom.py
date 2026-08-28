#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flash_rom.py - 把 .nes ROM 烧录到 BL616 UVC+UAC+NES 板的专用 Flash 分区，
               不触碰固件（固件仍在 0x0，ROM 在 0x100000）。

用途：换游戏时只烧 ROM 分区，无需重新编译/烧录固件。

用法：
    python tools/flash_rom.py <game.nes> [--port COM6] [--offset 0x100000]

流程：
    1. 校验 .nes 头部必须是 'NES\\x1a'。
    2. 生成临时 BLFlashCommand 配置（只写 ROM 分区、erase=1 仅擦该区）。
    3. 调用 BLFlashCommand.exe 烧录。

注意：板子需先进入下载模式（按住 BOOT -> 轻点 RESET -> 保持 BOOT），
      烧完再松 BOOT、按 RESET 启动。
"""
import sys
import os
import subprocess
import argparse

# ROM 分区的 Flash 物理偏移（必须与 infones_port.c 的 NES_ROM_FLASH_OFFSET 一致）
NES_ROM_FLASH_OFFSET = 0x100000
CHIP = "bl616"
DEFAULT_PORT = "COM6"

# 自动定位 BLFlashCommand.exe：优先 BL_SDK_BASE 环境变量，否则用常见默认路径
def find_blflash():
    base = os.environ.get("BL_SDK_BASE", "")
    candidates = []
    if base:
        candidates.append(os.path.join(base, "tools", "bflb_tools",
                                        "bouffalo_flash_cube", "BLFlashCommand.exe"))
    candidates.append(r"D:/bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube/BLFlashCommand.exe")
    candidates.append(r"C:/bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube/BLFlashCommand.exe")
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


def main():
    ap = argparse.ArgumentParser(description="Flash a .nes ROM to the BL616 ROM partition")
    ap.add_argument("nes", help="path to .nes ROM file")
    ap.add_argument("--port", default=DEFAULT_PORT, help="serial port (default COM6)")
    ap.add_argument("--offset", type=lambda x: int(x, 0), default=NES_ROM_FLASH_OFFSET,
                    help="flash offset (default 0x100000)")
    ap.add_argument("--blflash", default=None, help="path to BLFlashCommand.exe")
    args = ap.parse_args()

    nes = os.path.abspath(args.nes)
    if not os.path.isfile(nes):
        print("ERROR: %s 不存在" % nes)
        sys.exit(1)

    with open(nes, "rb") as f:
        hdr = f.read(4)
    if hdr != b"NES\x1a":
        print("ERROR: 不是有效的 iNES ROM（缺少 'NES\\x1a' 头部）")
        sys.exit(1)

    blflash = args.blflash or find_blflash()
    if not blflash:
        print("ERROR: 找不到 BLFlashCommand.exe，请用 --blflash 指定，或设置 BL_SDK_BASE 环境变量")
        sys.exit(1)

    # 把 .nes 临时复制为 .bin，避免 BLFlashCommand 对扩展名挑剔
    binpath = os.path.join(os.path.dirname(nes), "_rom_tmp.bin")
    with open(nes, "rb") as f:
        data = f.read()
    with open(binpath, "wb") as f:
        f.write(data)

    ini = os.path.join(os.path.dirname(os.path.abspath(__file__)), "flash_rom_tmp.ini")
    with open(ini, "w") as f:
        f.write("[FW]\n")
        f.write("filedir = %s\n" % binpath.replace("\\", "/"))
        f.write("address = 0x%X\n" % args.offset)
        f.write("erase = 1\n")

    print("烧录 %s -> flash 0x%X（仅 ROM 分区，固件不动）" % (nes, args.offset))
    try:
        rc = subprocess.call([blflash, "--port", args.port,
                              "--config", ini, "--chipname", CHIP, "write_flash_files"])
    finally:
        try:
            os.remove(ini)
        except OSError:
            pass
        try:
            os.remove(binpath)
        except OSError:
            pass
    if rc != 0:
        print("烧录失败，BLFlashCommand 返回 %d" % rc)
        sys.exit(rc)
    print("ROM 烧录成功。松开 BOOT、按 RESET 启动即可运行该游戏。")


if __name__ == "__main__":
    main()
