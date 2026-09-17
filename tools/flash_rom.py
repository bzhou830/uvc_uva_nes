#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flash_rom.py - 把 .nes ROM 当作"文件"管理到 BL616 的 ROM Flash 分区。

分区布局（与 infones_port.c 的目录区模型一致）：
    0x100000  目录区 ROM_DIR_SECTOR(4KB)
        每条目录项(44B) = 名字(32B UTF-8, NUL 结尾) + 偏移(4B) + 长度(4B)
                         + 标志(1B: 0空闲/1有效/2删除) + pad(3B)
        最多 ROM_DIR_MAX_ENTRIES=64 条
    0x101000  数据区：各 ROM 顺次存放，4KB 对齐

游戏名 = 烧录时用的文件名（去 .nes 后缀，可用 --name 覆盖）。名字随 ROM 一起
存进目录区，固件上电扫描目录建菜单并显示名字 —— 即"文件名=游戏名"。

PC 端用 rom_manifest.json 记录已烧录项，便于增量增/删；每次操作都会重写目录区。

子命令：
    add    <game.nes> [--name 名称]     追加一个游戏（名字默认取文件名）
    remove <名称>                       删除一个游戏（只改目录区，数据留作碎片）
    list                               列出当前已烧录的游戏
    reset                              清空目录区（菜单将显示"未找到游戏"）

通用参数：
    --port COM6    （默认 COM6）
    --blflash 路径 （可选，自动定位 BLFlashCommand.exe）

流程：
    1. 校验 .nes 头部必须是 'NES\\x1a'。
    2. 更新本地 manifest（记录 名字/长度/偏移）。
    3. 重写目录区 4KB（写 0x100000，erase=1）。
    4. 写新 ROM 数据（写其偏移，erase=1）。
板子需先进入下载模式（按住 BOOT -> 轻点 RESET -> 保持 BOOT），烧完松 BOOT 重启。
"""
import sys
import os
import json
import struct
import subprocess
import argparse
import re

# ---- Flash 分区布局常量（必须与 infones_port.c 保持一致）----
NES_ROM_FLASH_OFFSET = 0x100000
ROM_DIR_OFFSET       = NES_ROM_FLASH_OFFSET   # 目录区位于分区起点
ROM_DIR_SECTOR       = 0x1000          # 4KB 目录区
ROM_DATA_OFFSET      = NES_ROM_FLASH_OFFSET + ROM_DIR_SECTOR
ROM_DIR_MAX_ENTRIES  = 64
ROM_NAME_LEN         = 32
ROM_ENTRY_SZ         = ROM_NAME_LEN + 4 + 4 + 1 + 3   # 44 字节/项

# 当前固件已编译进的 mapper 集合：自动发现 nes/infones_core/mapper/ 下全部
# InfoNES_Mapper_*.c，与 gen_mapper.py 生成的 InfoNES_Mapper.c 保持一致。
# 烧录时据此给"固件可能跑不了"的 ROM 打告警。
def _discover_mappers():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    d = os.path.join(root, "nes", "infones_core", "mapper")
    s = set()
    if os.path.isdir(d):
        for fn in os.listdir(d):
            m = re.search(r"InfoNES_Mapper_(\d+)\.c$", fn)
            if m:
                s.add(int(m.group(1)))
    return s

SUPPORTED_MAPPERS = _discover_mappers() or {0, 1, 2, 3, 4}

CHIP         = "bl616"
DEFAULT_PORT = "COM6"

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST     = os.path.join(PROJECT_ROOT, "rom_manifest.json")


def find_blflash():
    base = os.environ.get("BL_SDK_BASE", "")
    cands = []
    if base:
        cands.append(os.path.join(base, "tools", "bflb_tools",
                                  "bouffalo_flash_cube", "BLFlashCommand.exe"))
    cands.append(r"D:/bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube/BLFlashCommand.exe")
    cands.append(r"C:/bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube/BLFlashCommand.exe")
    for c in cands:
        if os.path.isfile(c):
            return c
    return None


def load_manifest():
    if not os.path.isfile(MANIFEST):
        return []
    try:
        with open(MANIFEST, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, list):
            return data
    except Exception:
        pass
    return []


def save_manifest(entries):
    with open(MANIFEST, "w", encoding="utf-8") as f:
        json.dump(entries, f, ensure_ascii=False, indent=2)


def align4k(n):
    return (n + 0xFFF) & ~0xFFF


def next_data_offset(entries):
    """下一个可用数据偏移：数据区基址之后，按各项(偏移+4KB对齐长度)的最大值。"""
    max_end = ROM_DATA_OFFSET
    for e in entries:
        o = e.get("offset", ROM_DATA_OFFSET)
        max_end = max(max_end, align4k(o + e["length"]))
    return max_end


def build_dir_bin(entries):
    """把 manifest 项序列化成 4KB 目录区（flags=1 有效）。"""
    buf = bytearray(ROM_DIR_SECTOR)
    for i, e in enumerate(entries):
        if i >= ROM_DIR_MAX_ENTRIES:
            break
        nm = e["name"].encode("utf-8")[:ROM_NAME_LEN - 1]
        name_field = nm + b"\x00" * (ROM_NAME_LEN - len(nm))
        body = name_field + struct.pack("<II", e["offset"], e["length"])
        body += bytes([1, 0, 0, 0])          # flags=1 + pad
        buf[i * ROM_ENTRY_SZ:(i + 1) * ROM_ENTRY_SZ] = body
    return bytes(buf)


def flash_write(data, address, port, blflash):
    """写一段数据到指定 Flash 偏移（生成临时 ini 调 BLFlashCommand）。"""
    d = os.path.dirname(os.path.abspath(__file__))
    binpath = os.path.join(d, "_tmp_write.bin")
    inipath = os.path.join(d, "_tmp_write.ini")
    with open(binpath, "wb") as f:
        f.write(data)
    with open(inipath, "w") as f:
        f.write("[FW]\n")
        f.write("filedir = %s\n" % binpath.replace("\\", "/"))
        f.write("address = 0x%X\n" % address)
        f.write("erase = 1\n")
    try:
        return subprocess.call([blflash, "--port", port,
                                "--config", inipath, "--chipname", CHIP,
                                "write_flash_files"])
    finally:
        for p in (inipath, binpath):
            try:
                os.remove(p)
            except OSError:
                pass


def parse_nes_header(rom):
    """返回 (prg, chr, mapper, total)。total 是头声明的镜像总字节数。"""
    prg = rom[4]
    chr_ = rom[5]
    flags6 = rom[6]
    flags7 = rom[7]
    mapper = (flags6 >> 4) | (flags7 & 0xF0)
    trainer = 512 if (flags6 & 0x04) else 0
    total = 16 + trainer + prg * 16384 + chr_ * 8192
    return prg, chr_, mapper, total


def cmd_add(args):
    nes = os.path.abspath(args.nes)
    if not os.path.isfile(nes):
        print("ERROR: %s 不存在" % nes); sys.exit(1)
    with open(nes, "rb") as f:
        rom = f.read()
    if rom[:4] != b"NES\x1a":
        print("ERROR: 不是有效的 iNES ROM（缺少 'NES\\x1a' 头部）"); sys.exit(1)

    _, _, mapper, total = parse_nes_header(rom)
    # 防止像"双截龙.nes"这种尾带数百 KB 空格/填充的文件把 Flash 写爆：
    # 加载器只读头声明长度，故这里截断到 total，省空间且避免 manifest 长度歧义。
    if len(rom) > total:
        wasted = len(rom) - total
        rom = rom[:total]
        print("WARN: 文件比 iNES 头声明大 %d 字节（尾部位填充/损坏），已截断到 %d 字节。"
              % (wasted, total))
    elif len(rom) < total:
        print("ERROR: 文件 %d 字节 < 头声明 %d 字节，ROM 不完整/损坏。" % (len(rom), total))
        sys.exit(1)
    if mapper not in SUPPORTED_MAPPERS:
        print("WARN: mapper %d 未编译进当前固件，运行会失败。把 %d 加入 "
              "tools/gen_mapper.py 的 MAPPERS 并重编固件后可支持。" % (mapper, mapper))

    name = args.name or os.path.splitext(os.path.basename(nes))[0]
    blflash = args.blflash or find_blflash()
    if not blflash:
        print("ERROR: 找不到 BLFlashCommand.exe，用 --blflash 指定或设置 BL_SDK_BASE"); sys.exit(1)

    entries = load_manifest()
    # 同名先移除（offset 重算），保证名字唯一
    entries = [e for e in entries if e["name"] != name]
    offset = next_data_offset(entries)
    entries.append({"name": name, "length": len(rom), "offset": offset, "file": nes})
    save_manifest(entries)

    # 1) 重写目录区
    rc = flash_write(build_dir_bin(entries), ROM_DIR_OFFSET, args.port, blflash)
    if rc != 0:
        print("目录区烧录失败 rc=%d" % rc); sys.exit(rc)
    # 2) 写该 ROM 数据
    rc = flash_write(rom, offset, args.port, blflash)
    if rc != 0:
        print("ROM 数据烧录失败 rc=%d" % rc); sys.exit(rc)
    print("已添加 '%s' (mapper %d) -> flash 0x%X (%d 字节)，目录区已更新。"
          % (name, mapper, offset, len(rom)))
    print("松开 BOOT、按 RESET 启动，菜单即显示该游戏。")


def cmd_remove(args):
    entries = load_manifest()
    left = [e for e in entries if e["name"] != args.name]
    if len(left) == len(entries):
        print("未找到游戏 '%s'" % args.name); sys.exit(1)
    blflash = args.blflash or find_blflash()
    if not blflash:
        print("ERROR: 找不到 BLFlashCommand.exe"); sys.exit(1)
    save_manifest(left)
    rc = flash_write(build_dir_bin(left), ROM_DIR_OFFSET, args.port, blflash)
    if rc != 0:
        print("目录区烧录失败 rc=%d" % rc); sys.exit(rc)
    print("已删除 '%s'（数据区留作碎片，不影响其它游戏）。" % args.name)


def cmd_list(args):
    entries = load_manifest()
    if not entries:
        print("（暂无已烧录游戏）"); return
    print("已烧录 %d 个游戏：" % len(entries))
    for e in entries:
        print("  - %-16s 0x%X  %d 字节" % (e["name"], e["offset"], e["length"]))


def cmd_reset(args):
    blflash = args.blflash or find_blflash()
    if not blflash:
        print("ERROR: 找不到 BLFlashCommand.exe"); sys.exit(1)
    save_manifest([])
    rc = flash_write(b"\x00" * ROM_DIR_SECTOR, ROM_DIR_OFFSET, args.port, blflash)
    if rc != 0:
        print("目录区清空失败 rc=%d" % rc); sys.exit(rc)
    print("目录区已清空，菜单将显示\"未找到游戏\"。")


def main():
    ap = argparse.ArgumentParser(description="管理 BL616 ROM 分区里的 NES 游戏（类文件系统）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("add", help="追加一个游戏")
    a.add_argument("nes", help=".nes 文件路径")
    a.add_argument("--name", default=None, help="游戏名（默认取文件名去 .nes）")
    a.add_argument("--port", default=DEFAULT_PORT)
    a.add_argument("--blflash", default=None)
    a.set_defaults(func=cmd_add)

    r = sub.add_parser("remove", help="删除一个游戏")
    r.add_argument("name", help="游戏名")
    r.add_argument("--port", default=DEFAULT_PORT)
    r.add_argument("--blflash", default=None)
    r.set_defaults(func=cmd_remove)

    l = sub.add_parser("list", help="列出已烧录游戏")
    l.set_defaults(func=cmd_list)

    rs = sub.add_parser("reset", help="清空目录区")
    rs.add_argument("--port", default=DEFAULT_PORT)
    rs.add_argument("--blflash", default=None)
    rs.set_defaults(func=cmd_reset)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
