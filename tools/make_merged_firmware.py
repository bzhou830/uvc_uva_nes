#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_merged_firmware.py - 把"固件 + 所有已烧录 ROM"合并成单个可单刷镜像。

为什么需要它: 默认流程要分两步——先烧固件(@0x0)，再逐个 `flash_rom.py add` 把
游戏写进 ROM 分区(@0x100000)。对"发一版给别人直接刷"的场景，单文件更省事：
本脚本把固件 + 目录区 + 各 ROM 数据拼成一份连续镜像，用一个 BLFlashCommand
命令从 0x0 一次写完（详见 README §6.5）。

Flash 布局（与 infones_port.c 的目录区模型完全一致，目录项里的 offset 是
绝对 Flash 地址，因此合并镜像里 ROM 数据直接落在该绝对偏移即可）：

    [0x000000] 固件 (cherryusb_bl616.bin，长度不定)
    [0x100000] 目录区 (4KB，来自 rom_manifest.json，每条 44B)
    [0x101000] 数据区 (各 ROM 按 manifest 的 offset 落位，4KB 对齐)
    其余空隙填 0xFF（等同擦除态，不影响任何功能）

用法:
    python tools/make_merged_firmware.py
    python tools/make_merged_firmware.py --out build/build_out/cherryusb_bl616_merged.bin

依赖:
    - 复用 flash_rom.py 的 build_dir_bin / 布局常量（同目录 import）。
    - rom_manifest.json 必须存在且非空（先 `python tools/flash_rom.py add <game>`
      把所有游戏加进去；或拷一份已生成好的 manifest）。
    - 各 ROM 的 .nes 文件需存在（manifest 里记录了绝对路径）。
    - 固件需先 `make` 出来（build/build_out/cherryusb_bl616.bin）。
"""
import os
import sys
import argparse

import flash_rom as fr

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_OUT = os.path.join(PROJECT_ROOT, "build", "build_out")
DEFAULT_FW = os.path.join(BUILD_OUT, "cherryusb_bl616.bin")
DEFAULT_OUT = os.path.join(BUILD_OUT, "cherryusb_bl616_merged.bin")


def main():
    ap = argparse.ArgumentParser(
        description="合并固件+ROM 为单文件可单刷镜像")
    ap.add_argument("--fw", default=DEFAULT_FW, help="固件 bin 路径")
    ap.add_argument("--out", default=DEFAULT_OUT, help="输出合并 bin 路径")
    ap.add_argument("--manifest", default=fr.MANIFEST, help="rom_manifest.json 路径")
    args = ap.parse_args()

    if not os.path.isfile(args.fw):
        sys.stderr.write("ERROR: 固件不存在 %s（请先 make）\n" % args.fw)
        sys.exit(1)
    entries = fr.load_manifest()
    if not entries:
        sys.stderr.write("ERROR: %s 为空（请先 'python tools/flash_rom.py add <game>'）\n"
                         % args.manifest)
        sys.exit(1)

    fw = open(args.fw, "rb").read()
    dir_bin = fr.build_dir_bin(entries)

    # ROM 数据区结尾 = 所有 (offset + 对齐长度) 的最大值
    rom_end = fr.ROM_DATA_OFFSET
    for e in entries:
        rom_end = max(rom_end, fr.align4k(e["offset"] + e["length"]))

    total = max(len(fw),
                fr.NES_ROM_FLASH_OFFSET + fr.ROM_DIR_SECTOR,
                rom_end)
    # 整段先填 0xFF（擦除态），再覆盖有效区
    buf = bytearray(b"\xff" * total)
    buf[0:len(fw)] = fw                                   # 固件
    buf[fr.ROM_DIR_OFFSET: fr.ROM_DIR_OFFSET + len(dir_bin)] = dir_bin  # 目录区

    for e in entries:
        f = e.get("file")
        if not f or not os.path.isfile(f):
            sys.stderr.write("WARN: 找不到 ROM 文件 %s，跳过 '%s'\n" % (f, e["name"]))
            continue
        data = open(f, "rb").read()
        # 与 flash_rom.py add 保持一致：超过 iNES 头声明长度则截断（防尾部位填充）
        if data[:4] == b"NES\x1a":
            _, _, _, total_decl = fr.parse_nes_header(data)
            if len(data) > total_decl:
                data = data[:total_decl]
        buf[e["offset"]: e["offset"] + len(data)] = data

    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    with open(args.out, "wb") as fo:
        fo.write(buf)

    print("已生成合并固件: %s (%d 字节 = 0x%X)" % (args.out, len(buf), len(buf)))
    print("含 %d 个游戏:" % len(entries))
    for e in entries:
        print("  - %-16s 0x%X  %d 字节" % (e["name"], e["offset"], e["length"]))
    print("烧录命令:")
    print("  BLFlashCommand.exe --port COM6 --config flash_prog_cfg_merged.ini "
          "--chipname bl616 write_flash_files")


if __name__ == "__main__":
    main()
