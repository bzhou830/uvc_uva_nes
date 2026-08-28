#!/usr/bin/env python3
"""Prepare InfoNES core sources for the Bouffalo BL616 firmware.

The upstream InfoNES sources are C++ (.cpp) but are functionally plain C.
We copy them into nes/infones_core/ with a .c extension and rewrite the
mapper #includes so the whole tree compiles as C (no C++ runtime needed).
"""
import os, re, shutil

SRC = r"D:\bouffalo_sdk\examples\cherryusb\uvc_uac_nes\nes\infones\InfoNES-master\src"
DST = r"D:\bouffalo_sdk\examples\cherryusb\uvc_uac_nes\nes\infones_core"

os.makedirs(os.path.join(DST, "mapper"), exist_ok=True)

# headers
for h in ["InfoNES.h", "InfoNES_Types.h", "InfoNES_System.h",
          "InfoNES_Mapper.h", "InfoNES_pAPU.h", "K6502.h", "K6502_rw.h"]:
    shutil.copy(os.path.join(SRC, h), os.path.join(DST, h))

# top-level core .cpp -> .c
for c in ["InfoNES.cpp", "InfoNES_Mapper.cpp", "InfoNES_pAPU.cpp", "K6502.cpp"]:
    shutil.copy(os.path.join(SRC, c), os.path.join(DST, c[:-4] + ".c"))

# mapper/*.cpp -> mapper/*.c
src_mapper = os.path.join(SRC, "mapper")
for fn in sorted(os.listdir(src_mapper)):
    if fn.endswith(".cpp"):
        shutil.copy(os.path.join(src_mapper, fn),
                    os.path.join(DST, "mapper", fn[:-4] + ".c"))

# rewrite the mapper includes inside InfoNES_Mapper.c (.cpp -> .c)
mpath = os.path.join(DST, "InfoNES_Mapper.c")
with open(mpath, "r", encoding="utf-8", errors="replace") as f:
    txt = f.read()
txt2 = re.sub(r'#include\s+"mapper/InfoNES_Mapper_(\d+)\.cpp"',
              r'#include "mapper/InfoNES_Mapper_\1.c"', txt)
with open(mpath, "w", encoding="utf-8") as f:
    f.write(txt2)

print("Prepared core sources in", DST)
for root, _, files in os.walk(DST):
    for fn in sorted(files):
        if fn.endswith(".c") or fn.endswith(".h"):
            p = os.path.join(root, fn)
            print(" ", os.path.relpath(p, DST), os.path.getsize(p), "bytes")
