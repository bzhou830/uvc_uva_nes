#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rom_flash_gui.py - BL616 NES ROM 烧录图形界面工具

功能:
  1. 选择游戏 ROM: 列出 tools/roms 下的 .nes, 也可"浏览"选任意文件。
  2. 选择 COM 端口: 自动枚举本机串口(优先 pyserial, 否则 PowerShell 回退), 可手填。
  3. 点击"烧录": 复用 flash_rom.py add, 把所选 ROM 写入 BL616 的 ROM 分区(0x100000)。

使用:
  python rom_flash_gui.py
  (需 Windows + BLFlashCommand.exe 在 D:/bouffalo_sdk/... 或设置 BL_SDK_BASE 环境变量)

说明:
  - 板子需先进入下载模式(按住 BOOT -> 轻点 RESET -> 保持 BOOT 约 2 秒), 烧完松开 BOOT 按 RESET 启动。
  - 本工具只写 ROM 分区, 固件(cherryusb_bl616.bin @0x0)需先烧好。
  - 串口枚举优先用 pyserial; 若未安装则回退到 PowerShell, 仍可直接手填 COMx。
"""
import os
import sys
import subprocess
import threading
import re

import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext, messagebox

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
ROMS_DIR = os.path.join(SCRIPT_DIR, "roms")
FLASH_ROM = os.path.join(SCRIPT_DIR, "flash_rom.py")
DEFAULT_PORT = "COM6"

# 当前固件已编译进的 mapper 集合：自动发现 nes/infones_core/mapper/ 下全部
# InfoNES_Mapper_*.c，与 gen_mapper.py 生成的 InfoNES_Mapper.c 保持一致。
def _discover_mappers():
    d = os.path.join(PROJECT_ROOT, "nes", "infones_core", "mapper")
    s = set()
    if os.path.isdir(d):
        for fn in os.listdir(d):
            m = re.search(r"InfoNES_Mapper_(\d+)\.c$", fn)
            if m:
                s.add(int(m.group(1)))
    return s

SUPPORTED_MAPPERS = _discover_mappers() or {0, 1, 2, 3, 4}


# ----------------------------- COM 端口枚举 -----------------------------
def _dedup(items):
    seen = set()
    out = []
    for it in items:
        if it not in seen:
            seen.add(it)
            out.append(it)
    return out


def list_com_ports():
    """返回串口标签列表(如 'COM6  (USB Serial Port)')。失败则回退到手填 COM6。"""
    ports = []
    # 1) pyserial(首选, 带描述)
    try:
        import serial.tools.list_ports as lp
        for p in lp.comports():
            if p.description and p.description != p.device:
                ports.append("%s  (%s)" % (p.device, p.description))
            else:
                ports.append(p.device)
        if ports:
            return _dedup(ports)
    except Exception:
        pass
    # 2) Windows PowerShell 回退(按 OEM 代码页解码, 兼容中文端口名)
    try:
        raw = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command",
             "Get-CimInstance -ClassName Win32_SerialPort | "
             "ForEach-Object { $_.DeviceID + '  (' + $_.Name + ')' }"],
            stderr=subprocess.DEVNULL, timeout=10)
        try:
            out = raw.decode("oem", errors="replace")
        except Exception:
            out = raw.decode(errors="replace")
        for line in out.splitlines():
            line = line.strip()
            if line:
                ports.append(line)
    except Exception:
        pass
    if not ports:
        ports.append(DEFAULT_PORT)
    return _dedup(ports)


def port_device(label):
    """从标签里取出纯设备名(如 'COM6')。"""
    return label.split()[0] if label else ""


# ----------------------------- 小工具 -----------------------------
def parse_nes(path):
    """返回 (prg, chr, mapper) 或 None(非有效 iNES)。"""
    try:
        with open(path, "rb") as f:
            h = f.read(16)
        if len(h) < 16 or h[:4] != b"NES\x1a":
            return None
        prg, chr_ = h[4], h[5]
        mapper = (h[6] >> 4) | (h[7] & 0xF0)
        return prg, chr_, mapper
    except Exception:
        return None


# ----------------------------- 主界面 -----------------------------
class FlashGUI:
    def __init__(self, root):
        self.root = root
        self.busy = False
        self.selected_rom = tk.StringVar()
        self.port_var = tk.StringVar(value=DEFAULT_PORT)
        self.status_var = tk.StringVar(value="就绪。请选择 ROM 与 COM 端口后点\"烧录\"。")

        root.title("BL616 NES ROM 烧录工具")
        root.geometry("660x600")
        root.resizable(True, True)
        self._build_ui()
        self.refresh_roms()
        self.refresh_ports()

    # ------------------------ UI 构建 ------------------------
    def _build_ui(self):
        # 顶部: ROM 选择
        f_rom = ttk.LabelFrame(self.root, text="1. 选择游戏 ROM", padding=8)
        f_rom.pack(fill="x", padx=10, pady=(10, 4))

        f_rom.rowconfigure(0, weight=1)
        self.rom_list = tk.Listbox(f_rom, height=7, exportselection=False)
        self.rom_list.grid(row=0, column=0, rowspan=3, sticky="nsew", padx=(0, 8))
        sb = ttk.Scrollbar(f_rom, orient="vertical", command=self.rom_list.yview)
        sb.grid(row=0, column=1, rowspan=3, sticky="ns")
        self.rom_list.config(yscrollcommand=sb.set)
        self.rom_list.bind("<<ListboxSelect>>", self.on_rom_select)

        ttk.Button(f_rom, text="浏览...", command=self.browse_rom,
                   width=12).grid(row=0, column=2, sticky="ew", pady=(0, 4))
        ttk.Button(f_rom, text="刷新列表", command=self.refresh_roms,
                   width=12).grid(row=1, column=2, sticky="ew", pady=(0, 4))

        self.rom_info = ttk.Label(f_rom, text="未选择 ROM", foreground="#555")
        self.rom_info.grid(row=2, column=2, sticky="w")

        # 中部: COM 端口
        f_com = ttk.LabelFrame(self.root, text="2. 选择 COM 端口", padding=8)
        f_com.pack(fill="x", padx=10, pady=4)

        ttk.Label(f_com, text="端口:").grid(row=0, column=0, sticky="w")
        self.port_combo = ttk.Combobox(f_com, textvariable=self.port_var,
                                       width=30, state="readonly")
        self.port_combo.grid(row=0, column=1, sticky="ew", padx=(6, 6))
        ttk.Button(f_com, text="刷新", command=self.refresh_ports,
                   width=10).grid(row=0, column=2, sticky="ew")
        f_com.columnconfigure(1, weight=1)

        # 操作区
        f_act = ttk.Frame(self.root, padding=(10, 4))
        f_act.pack(fill="x", padx=0, pady=2)
        self.flash_btn = ttk.Button(f_act, text="烧录", command=self.do_flash,
                                    width=14)
        self.flash_btn.pack(side="left", padx=(0, 8))
        ttk.Button(f_act, text="列出已烧录", command=self.do_list,
                   width=14).pack(side="left", padx=(0, 8))
        ttk.Button(f_act, text="清空分区", command=self.do_reset,
                   width=14).pack(side="left")

        # 日志
        f_log = ttk.LabelFrame(self.root, text="日志", padding=8)
        f_log.pack(fill="both", expand=True, padx=10, pady=(4, 4))
        self.log_text = scrolledtext.ScrolledText(f_log, height=12,
                                                  state="normal", wrap="word")
        self.log_text.pack(fill="both", expand=True)

        # 状态栏
        ttk.Label(self.root, textvariable=self.status_var,
                  relief="sunken", anchor="w",
                  padding=(8, 4)).pack(fill="x", side="bottom")

    # ------------------------ ROM 列表 ------------------------
    def refresh_roms(self):
        self.rom_list.delete(0, tk.END)
        if os.path.isdir(ROMS_DIR):
            for fn in sorted(os.listdir(ROMS_DIR)):
                if fn.lower().endswith(".nes"):
                    self.rom_list.insert(tk.END, fn)
        if self.rom_list.size() == 0:
            self.rom_list.insert(tk.END, "(tools/roms 下无 .nes)")
            self.rom_list.config(state="disabled")
        else:
            self.rom_list.config(state="normal")

    def on_rom_select(self, evt):
        sel = self.rom_list.curselection()
        if not sel:
            return
        name = self.rom_list.get(sel[0])
        path = os.path.join(ROMS_DIR, name)
        if not os.path.isfile(path):
            return
        self._set_rom(path)

    def browse_rom(self):
        path = filedialog.askopenfilename(
            title="选择 NES ROM",
            filetypes=[("NES ROM", "*.nes"), ("All", "*.*")])
        if path:
            self._set_rom(path)

    def _set_rom(self, path):
        self.selected_rom.set(path)
        size = os.path.getsize(path)
        info = "已选: %s\n大小: %d 字节" % (os.path.basename(path), size)
        meta = parse_nes(path)
        if meta is None:
            info += "\n警告: 不是有效 iNES 文件!"
        else:
            prg, chr_, mapper = meta
            info += "\nPRG:%d CHR:%d Mapper:%d" % (prg, chr_, mapper)
            if mapper not in SUPPORTED_MAPPERS:
                info += "  [未编译进固件, 运行会失败]"
        self.rom_info.config(text=info, foreground="#333")

    # ------------------------ COM 端口 ------------------------
    def refresh_ports(self):
        ports = list_com_ports()
        self.port_combo["values"] = ports
        cur = self.port_var.get()
        if cur not in ports:
            # 若当前值(如手填的 COM6)不在列表中, 追加进去
            ports2 = list(ports) + [cur]
            self.port_combo["values"] = ports2
            self.port_combo.set(cur)
        else:
            self.port_combo.set(cur)

    # ------------------------ 动作 ------------------------
    def do_flash(self):
        if self.busy:
            return
        rom = self.selected_rom.get()
        if not rom or not os.path.isfile(rom):
            messagebox.showerror("错误", "请先选择一个有效的 .nes ROM")
            return
        port = port_device(self.port_var.get())
        if not port:
            messagebox.showerror("错误", "请选择或输入 COM 端口")
            return
        self.busy = True
        self.flash_btn.config(state="disabled")
        self.set_status("烧录中... 端口 %s, ROM %s"
                        % (port, os.path.basename(rom)))
        threading.Thread(target=self._run_flash, args=(rom, port),
                         daemon=True).start()

    def _run_flash(self, rom, port):
        try:
            cmd = [sys.executable, FLASH_ROM, "add", rom, "--port", port]
            self.append_log(">>> " + " ".join(cmd) + "\n")
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True,
                                    cwd=SCRIPT_DIR, bufsize=1)
            for line in proc.stdout:
                self.append_log(line)
            rc = proc.wait()
            self.append_log("\n=== 退出码 rc=%d ===\n" % rc)
            if rc == 0:
                self.set_status("烧录完成 (rc=0)。松开 BOOT、按 RESET 启动板子。")
            else:
                self.set_status("烧录失败 rc=%d, 详见上方日志。" % rc)
        except Exception as e:
            self.append_log("异常: %s\n" % e)
            self.set_status("烧录异常: %s" % e)
        finally:
            self.busy = False
            self.root.after(0, lambda: self.flash_btn.config(state="normal"))

    def do_list(self):
        if self.busy:
            return
        self.busy = True
        threading.Thread(target=self._run_simple,
                         args=(["list"], "列出已烧录(本地 manifest)"),
                         daemon=True).start()

    def do_reset(self):
        if self.busy:
            return
        if not messagebox.askyesno("确认",
                                    "将清空 ROM 分区目录区, 菜单会显示\"未找到游戏\"。继续?"):
            return
        port = port_device(self.port_var.get()) or DEFAULT_PORT
        self.busy = True
        threading.Thread(target=self._run_simple,
                         args=(["reset", "--port", port], "清空分区"),
                         daemon=True).start()

    def _run_simple(self, subcmd, label):
        try:
            cmd = [sys.executable, FLASH_ROM] + subcmd
            self.append_log("\n--- %s ---\n>>> %s\n" % (label, " ".join(cmd)))
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True,
                                    cwd=SCRIPT_DIR, bufsize=1)
            for line in proc.stdout:
                self.append_log(line)
            rc = proc.wait()
            self.append_log("=== rc=%d ===\n" % rc)
        except Exception as e:
            self.append_log("异常: %s\n" % e)
        finally:
            self.busy = False

    # ------------------------ 日志/状态 ------------------------
    def append_log(self, text):
        self.root.after(0, self._append, text)

    def _append(self, text):
        self.log_text.insert(tk.END, text)
        self.log_text.see(tk.END)

    def set_status(self, text):
        self.root.after(0, self.status_var.set, text)


def main():
    if not os.path.isfile(FLASH_ROM):
        # 允许独立运行(找不到 flash_rom.py 时给出提示, 但仍可看 UI)
        print("WARNING: 未找到 %s, 烧录功能不可用。" % FLASH_ROM)
    root = tk.Tk()
    FlashGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
