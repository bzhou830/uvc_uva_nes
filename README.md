# BL616 UVC + UAC 复合设备 — NES 模拟器串流项目

> 在 BL616（bl616dk）上把 **InfoNES** 模拟器跑起来，NES 画面经 **BL616 硬件 MJPEG 编码器** 通过 **UVC（USB 摄像头）** 传出，NES APU 声音经 **UAC（USB 麦克风）** 传出。主机（Windows 相机 / OBS / 录制软件）把它识别为一个“带麦克风的摄像头”，即可看画面、录声音。
>
> 本文档为**端到端工程文档**（原理 / SDK / 编译 / 烧录 / 测试 / 平台），按 ai-thinker 风格整理。所有修改都只在本工程 app 代码，不碰 SDK（详见 §14 工程约定）。

---

## 1. 硬件平台

| 项目  | 说明                                                 |
| --- | -------------------------------------------------- |
| 芯片  | Bouffalo Lab **BL616**（RISC-V，320 MHz，**无 FPU**）     |
| 开发板 | bl616dk（BL616 官方/兼容开发板）                            |
| 内存  | 内部 SRAM 约 **415 KB**；板载 **4 MB PSRAM**（`0xA8000000` 起） |
| USB | 内置 USB 2.0（Full-Speed Device），作 Composite Device    |
| 串口  | 板载 USB-SERIAL（CH340），本工程烧录口 **COM6**               |

> **BL616 有硬件 MJPEG 编码器**（RISC-V 无 FPU，但芯片内有独立 MJPEG 块）。MJPEG 的 DCT / 量化 / 哈夫曼由硬件完成并直接吐 baseline JPEG；只有 **RGB565→YUYV 的色彩转换**在软件里做（逐像素整数运算，很便宜）。所有大缓冲（NES 帧、YUYV、JPEG、音频环、ROM）都放在 PSRAM，SRAM 几乎只留栈与少量静态量。

---

## 2. 整体架构

```
                ┌─────────────────────────────────────────────┐
                │              BL616 (bl616dk)                 │
                │                                              │
  NES ROM ──► InfoNES 核心 ──┬──► PPU ──► WorkFrame(RGB565)    │
  (flash)     (K6502+APU)    │       256×240                  │
                              │            │                   │
                              │            ▼                   │
                              │  hw_mjpeg_encode (HW MJPEG)    │
                              │  RGB565→YUYV 4:2:2 软件转换    │
                              │  （2× 放大在转换内顺带完成）     │
                              │            │  MJPEG            │
                              │            ▼                   │
                              │  yuyv_buf→jpeg_buf (PSRAM)     │
                              │            │                   │
                              │            ▼                   │
                              │      UVC 视频流 (ep 0x81) ─────┼──► USB ──► 主机摄像头
                              │                              │
                              └──► APU ──► InfoNES_SoundOutput │
                                  5 路波形混音 + 去直流          │
                                  │ PCM int16                   │
                                  ▼                            │
                          nes_audio_ring (PSRAM)               │
                                  │                            │
                                  ▼                            │
                          UAC 麦克风流 (ep 0x82) ──────────────┼──► USB ──► 主机录音
                                                              │
   空闲兜底（NES 未跑时）：视频=彩条 MJPEG，音频=1 kHz 正弦测试音      │
                └─────────────────────────────────────────────┘
```

**一个 USB 设备，两个功能类**（用 IAD 复合，`bDeviceClass=0xEF / SubClass=0x02 / Protocol=0x01`）：

- **UVC**：VideoControl(iface0) + VideoStream(iface1, iso IN ep 0x81)，格式 MJPEG，分辨率 **512×480 / 15 fps**（默认开 2× 放大；关 `NES_FRAME_UPSCALE_2X` 退回 256×240）。
- **UAC**：AudioControl(iface2) + AudioStream 麦克风(iface3, iso IN ep 0x82)，**44100 Hz / 立体声 / 16-bit**。

> **默认为什么是 512×480？** NES 原生分辨率是 256×240，本工程默认开 `NES_FRAME_UPSCALE_2X`（最近邻 2× 放大）输出 512×480。放大在 `hw_mjpeg_encode` 的 RGB565→YUYV 转换里顺带完成（每个 NES 像素展成 2×2 的 YUYV 块），**不额外占 RGB565 大缓冲**；目的不是“更清晰”，而是让色度正好落在 NES 像素上，消除 Windows `usbvideo.sys` 把 MJPEG 解压成 4:2:2 时高对比边缘的红/绿鬼影（因为 DCT 是硬件做，2× 几乎不增加 CPU 成本）。详见 §4.2。

### 任务与数据流

| 任务 | 优先级 | 职责 |
| --- | --- | --- |
| `uvc_uac_task` | 5（高） | USB 复合设备初始化、轮询 `usbd_event_handler`、UVC/UAC 端点传输调度 |
| `nes_task` | 4（低） | 跑 `InfoNES_Main()` 模拟循环：每帧 PPU 渲染 → `InfoNES_LoadFrame()` 编码 → APU 混音 → `InfoNES_Wait()` 让出 |

视频采用**单缓冲 + `jpeg_state` 门控**（BL616 单核，状态位即可避免竞争）：

```
nes_task:  InfoNES_LoadFrame()  if(jpeg_state==0){ 编码→jpeg_buf; jpeg_state=1 }
uvc_task:  nes_bridge_get_video() if(jpeg_state==1){ 取走; jpeg_state=2 }
uvc_iso:  传输完成回调 nes_bridge_release_video() if(jpeg_state==2){ jpeg_state=0 }
```

因此**编码与 USB 传输严格串行**：`enc fps`（串口每秒打印的编码帧率）= `min(编码速率, USB 消费速率)`。这一性质是性能分析的关键（见 §10）。

---

## 3. 目录结构

```
uvc_uac_nes/
├── main.c                  # 建 uvc_uac_task + nes_task，调 uvc_uac_init / poll
├── usb_composite.c         # UVC+UAC 描述符、端点、轮询（video/audio 源回调）
├── uvc_uac.h               # 接口/端点/分辨率/采样率 常量与源 API 声明
├── frame_audio_source.c    # 实现 video_source_get_frame / audio_source_fill
├── flash_prog_cfg.ini      # 默认烧录配置
├── flash_prog_cfg_com6.ini # 固件烧录配置（COM6，写 flash 0x0）
├── flash_prog_cfg_rom.ini  # ROM 分区烧录配置（写 flash 0x100000，不擦固件）
├── CMakeLists.txt          # InfoNES 各源文件 + 热点 -O2
├── nes/
│   ├── hw_mjpeg.c/.h       # BL616 硬件 MJPEG 编码器封装（RGB565→YUYV→HW encode）
│   ├── jpeg_head.c/.h      # JPEG 文件头生成（DQT/SOF/SOS），YUV_MODE_422
│   ├── soft_mjpeg.c/.h     # [兜底] 软件定点整数 baseline JPEG 编码器（仅测试图案 fallback）
│   ├── infones_port.c      # InfoNES 可移植 API 实现 + NES↔USB 桥接
│   ├── infones_port.h
│   └── infones_core/       # InfoNES 核心（InfoNES.c / K6502.c / InfoNES_Mapper.c / InfoNES_pAPU.c / 调色板）
└── tools/
    ├── flash_rom.py        # 把 .nes 烧到 ROM 分区（换游戏只烧这一步，免重编固件）
    ├── gen_mapper.py       # 生成 InfoNES_Mapper.c（MAPPERS 控制编进哪些 mapper）
    ├── make_demo_rom.py    # 生成纯色 demo ROM（调色板 smoke-test 用）
    ├── nes2c.py            # [废弃] 旧流程 .nes → nes_rom.c（现 ROM 走 Flash 分区，不再需要）
    ├── gen_tables.py       # 辅助：生成 LUT/常量表
    ├── prep_infones.py     # 辅助：预处理 InfoNES 核心
    ├── soft_jpeg_oracle.py / validate_host_jpg.py / host_test.*  # 辅助：主机侧 JPEG 比对/校验
```

---

## 4. 关键原理

### 4.1 视频：NES PPU → MJPEG → UVC

1. InfoNES 每渲染完一帧，把像素按 `NesPalette[]`（RGB565）写入 `WorkFrame[256×240]`。
2. `InfoNES_LoadFrame()` 把 `WorkFrame` 交给 `hw_mjpeg_encode()`（RGB565→YUYV 4:2:2 转换 + BL616 硬件 MJPEG 编码）编码成 MJPEG，写入 `jpeg_buf`，置 `jpeg_state=1`。
3. USB 视频 ISO 回调传完一帧后调用 `nes_bridge_release_video()`，把 `jpeg_state` 复位为 0，允许下一帧重编码。
4. `uvc_uac_poll()` 在 `video_streaming && !video_busy` 时取帧并启动 `usbd_video_stream_start_write()`。

**硬件 MJPEG 封装（`nes/hw_mjpeg.c`）每帧流程：**

```c
// 1. 软件 RGB565 → YUYV（BT.601 limited range），2× 放大在此顺带完成
// 2. dcache_clean(yuyv_buf)            // PSRAM 可缓存，HW 从它 DMA 读取
// 3. bflb_mjpeg_update_input_output_buff(dev, yuyv, NULL, out, out_size)
//    bflb_mjpeg_sw_run(dev, 1)         // 内存输入模式（无需 DVP 摄像头）
// 4. while(!(get_intstatus & MJPEG_INTSTS_ONE_FRAME));  // 轮询完成
//    bflb_mjpeg_int_clear(dev, MJPEG_INTCLR_ONE_FRAME)
// 5. len = bflb_mjpeg_get_frame_info(dev, &pic)        // 硬件自动加 JPEG 头 + EOI
//    bflb_mjpeg_pop_one_frame(dev); bflb_mjpeg_stop(dev)
// 6. dcache_invalidate_range(out, len)  // 让 CPU 看到硬件写回的 JPEG
```

**编码器要点（容易踩坑的地方）：**

- **硬件 MJPEG，输出 4:2:2**：BL616 的 MJPEG 块吃 `YUV422_YUYV` 内存缓冲、吐 baseline JPEG（4:2:2）。所以本工程**不走软件 4:4:4**，而是先把 NES 的 RGB565 转成 YUYV（4:2:2，软件做、很便宜），再交给硬件编码。色度由相邻两像素**居中平均**（在 2× 放大下，两像素是同一 NES 像素的拷贝 → 色度正好落在 NES 像素上）。
- **BT.601 studio / limited range（Y∈[16,235]，Cb/Cr∈[16,240]）**：UVC/MJPEG 在 Windows 上由 `usbvideo.sys` 按 limited range 解码。RGB565→YUYV 若用 full-range（Y∈[0,255]），宿主会再把它拉伸一次 → **暗部压暗、低/中亮度颜色整体偏移**（“不同颜色偏的程度不一样”的典型症状）。本项目已用 studio-range 修正。
- **`SOFT_MJPEG_QUALITY = 90`**：硬件编码质量参数（等价量化表步进）。质量越高，文字等高频细节保留越好。太低（如 60）会丢高频 → 文字发虚。质量 90 的 MJPEG 单帧约 15–60 KB（512×480），`MJPEG_BUF_SZ`(256 KB) 缓冲足够。
- **缓存一致性**：YUYV 输入缓冲在 PSRAM（可缓存），编码前 `bflb_l1c_dcache_clean_range()`；硬件写回 JPEG 后 `bflb_l1c_dcache_invalidate_range()`，否则 CPU 会读到陈旧数据。

### 4.2 色彩转换与色度对齐（红/绿鬼影消除的数学）

**RGB565 → YCbCr（BT.601 studio range，整数，位于 `hw_mjpeg.c:rgb565_ycbcr`）：**

```c
static inline void rgb565_ycbcr(uint16_t p, int *y, int *cb, int *cr)
{
    int r = (p >> 11) & 0x1F; r = (r << 3) | (r >> 2);   // 5→8 bit
    int g = (p >> 5)  & 0x3F; g = (g << 2) | (g >> 4);   // 6→8 bit
    int b = p         & 0x1F; b = (b << 3) | (b >> 2);   // 5→8 bit
    *y  = 16  + (( 66 * r + 129 * g +  25 * b) >> 8);
    *cb = 128 + ((-38 * r -  74 * g + 112 * b) >> 8);
    *cr = 128 + ((112 * r -  94 * g -  18 * b) >> 8);
}
```

系数即标准 BT.601（Rec.601）常量：`Y   =  0.257R+0.504G+0.098B`，`Cb = -0.148R-0.291G+0.439B`，`Cr = 0.439R-0.368G-0.071B`，缩放 256 后取整。

**YUYV 打包（4:2:2，每两个像素共享一组 Cb/Cr）：**

```
字节序： [Y0][Cb][Y1][Cr]  [Y2][Cb][Y3][Cr] ...
              ↑ Cb/Cr 为像素0、1 的居中平均
```

**为什么 2× 放大能消除红/绿鬼影（核心）：**

- Windows `usbvideo.sys` 把 MJPEG 解成 4:2:2 YUY2 后做**共址（cosited）色度**：每个 Cb/Cr 归属“偶数列像素”，奇数列像素的色度由相邻偶数插值得到。
- **不放大（256×240 真 4:4:4 或 4:2:2）**：NES 相邻像素是不同内容（如 CONTRA 标题字的红边与绿边相邻），主机在偶数列重新采样色度 → 高对比彩色边缘出现红/绿鬼影。这是主机端固有限制，纯软件 256×240 无法消除。
- **2× 放大（默认）**：`hw_mjpeg_encode` 把每个 NES 像素展成 2×2 YUYV 块，且色度取该 NES 像素自身（= 相邻两拷贝像素的居中平均，恒等）：
  ```
  NES 像素 P ──► 输出 [Y][Cb][Y][Cr]  (第 2ny 行)
               输出 [Y][Cb][Y][Cr]  (第 2ny+1 行)   // 两行、每行两像素都是 P 的拷贝
  ```
  于是主机在偶数列采到的色度 **正好是该 NES 像素的正确色度**，奇数列插值也落在同一 NES 像素上 → **红/绿鬼影消失、文字/边缘锐利**。因为 DCT/量化/哈夫曼是硬件做，2× 几乎不增加 CPU 成本。

> 2× 转换的**去冗余写法**（性能优化 ②，见 §10.3）：直接按 NES 像素遍历，每像素只算一次 `(Y,Cb,Cr)` 填 2×2 块，比“按输出像素遍历、每对重复算 4 次”少 4× 的 `rgb565_ycbcr` 调用（245760 → 61440）。

### 4.3 音频：NES APU → PCM → UAC

1. InfoNES 主机循环每帧（vsync）调 `InfoNES_pAPUVsync()` → `InfoNES_SoundOutput()`。
2. `InfoNES_SoundOutput()` 把 5 路 APU 波形（2 脉冲 + 三角 + 噪声 + DPCM）混合后写入 `nes_audio_ring`（PSRAM 上的 SPSC 环形缓冲）。
3. `nes_bridge_get_audio()` 在 `audio_source_fill()` 中被 UAC 轮询取走，送 `usbd_ep_start_write()`。
4. **空闲兜底**：NES 未运行时，`audio_source_fill()` 改发 **1 kHz 正弦测试音**，保证 UAC 麦克一直有声（便于确认枚举/录音）。

**音频最重要的一处修正（本项目初版是 bug）：**

- InfoNES 的 pAPU 渲染出的波形是**单极性**的（每样本 ≥ 0，5 路求和约 0..9307）。
- 初版用 `GAIN=400` 直接乘 → 严重削波，且输出是“整流状”的单极性信号，**听着刺耳/失真**。
- 修正（`nes/infones_port.c:InfoNES_SoundOutput`）：先过**单极点去直流（DC blocker）**把信号拉成正确的双极性，再乘可耳调的 `NES_AUDIO_GAIN`（默认 40）并削波保护。`L/R` 直接复制（NES 是单声道）：

```c
static int dc_est = 0;
int sum = (int)w1[i] + (int)w2[i] + (int)w3[i] + (int)w4[i] + (int)w5[i];
int ac  = sum - dc_est;                       // 去除 DC 偏置
dc_est += (sum - dc_est) >> NES_AUDIO_DC_SHIFT; // 一阶跟踪均值（极点 ~30Hz）
int s = ac * NES_AUDIO_GAIN;
s = clamp(s, -32768, 32767);
```

> APU 采样率固定在 44100 Hz（`ApuQuality=2`，`samples_per_sync=735`），与 UAC 端点 44100 完全对齐，无需重采样。

### 4.4 调色板

`NesPalette[64]` 用 **FCEUX 标准调色板**（RGB565，R5G6B5）。InfoNES 把 `NesPalette` 直接拷进 `PalTable[]` 再进 `WorkFrame`，所以改这个数组就改全局颜色。早期 InfoNES 默认调色板偏蓝暗，已替换。

---

## 5. 编译

### 5.1 环境

- Bouffalo SDK（路径记为 `D:/bouffalo_sdk`）。
- SDK 自带 mingw `make` 与 `cmake`；工具链 `toolchain_gcc_t-head_windows`。
- `defconfig` 需开启：
  - `CONFIG_PSRAM = y`（4 MB PSRAM 初始化与堆注册）
  - `CONFIG_COREDUMP = n`（关掉崩溃时的 base64 coredump 刷屏，便于看干净日志；可选但强烈建议）

### 5.2 构建命令

```bash
cd D:/bouffalo_sdk/examples/cherryusb/uvc_uac_nes
export BL_SDK_BASE=D:/bouffalo_sdk CHIP=bl616 BOARD=bl616dk
make            # SDK 的 cmake 会重读 defconfig 自动注入，无需 rm build
```

> **路径坑**：SDK 自带 mingw `make` 不翻译 `/d/...`，`BL_SDK_BASE` 必须用 **Windows 风格** `D:/bouffalo_sdk`。

产物（在 `build/build_out/`）：

- `cherryusb_bl616.bin`（主固件，要烧的就是它）
- `.ota` / `.elf` / `.map`
- `.xz` 会报 “ota file exceeds partition table size limit” —— **无害**，不影响要烧的 `.bin`。

### 5.3 构建标志（性能相关）

SDK 工程默认是 **`-Os`（优化体积）**。InfoNES 的 6502 解释器 + PPU 渲染、以及逐像素 RGB565→YUYV 是 ALU/分支密集代码，在 `-Os` 下明显偏慢。本工程对热点翻译单元单独加 `-O2`（`CMakeLists.txt`，per-file `COMPILE_FLAGS` 在命令行最后、`-O2` 覆盖 `-Os`）：

```cmake
set_source_files_properties(
    ./nes/infones_core/K6502.c
    ./nes/infones_core/InfoNES.c
    ./nes/infones_core/InfoNES_Mapper.c
    ./nes/infones_core/InfoNES_pAPU.c
    ./nes/infones_port.c
    ./nes/hw_mjpeg.c
    ./frame_audio_source.c
    PROPERTIES COMPILE_FLAGS "-O2"
)
```

验证方法：编译后查 `build/CMakeFiles/app.dir/build.make` 中上述 `.c.obj` 的编译命令行含 `-O2`；`flags.make` 其余文件仍为 `-Os`。详见 §10 性能记录。

---

## 6. 烧录

### 6.1 烧录工具

`D:/bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube/BLFlashCommand.exe`（Windows 原生，需要能访问 COM 口）。

`flash_prog_cfg_com6.ini`：`[FW] filedir = ./build/build_out/cherryusb*_bl616*.bin; address=0x000000; erase=1`。

### 6.2 进入下载模式（必须手动）

工具的“断电控制 DTR”在这块板子上**不会复位芯片**，所以必须手动：

1. **按住 BOOT**（保持低电平）；
2. 轻点 **RESET**（BOOT 为低时复位进 bootrom）；
3. **保持按住 BOOT**，立刻跑烧录命令；
4. 看到 `Handshake succeeded` / `Start to erase` 后**松开 BOOT**；
5. 烧完 `Verification succeeded` / RC=0。

> 烧录前先**关闭占用 COM6 的串口监视器 / Putty / IDE 终端**，否则 `PermissionError(拒绝访问)`。

### 6.3 烧录命令

```bash
cd D:/bouffalo_sdk/examples/cherryusb/uvc_uac_nes
D:/bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube/BLFlashCommand.exe \
  --port COM6 --config flash_prog_cfg_com6.ini --chipname bl616 write_flash_files
```

> **固件只需烧一次**。之后换游戏只烧 ROM 分区（`0x100000`），用 `tools/flash_rom.py`，**无需重编固件** —— 见 §11。

### 6.4 启动

烧完**松开 BOOT，再按一下 RESET**（或重插 USB）让新固件启动。串口 115200 8N1 应看到：

```
[OS] start scheduler
[UVCA] task start
[UVCA] uvc_uac_init enter ...
[UVCA] UVC+UAC composite device initialized
[NES] init: load ROM from flash
[NES] ROM loaded from flash @0x100000, len=16400 (PRG=1 CHR=0 trainer=0)
[NES] init: PSRAM memset done
[NES] Load ret=0
[NES] Main start
```

---

## 7. 测试

| 项目     | 方法                             | 预期                                                  |
| ------ | ------------------------------ | --------------------------------------------------- |
| USB 枚举 | 设备管理器                          | 出现 “USB Video Device”（摄像头）+ “USB Audio Device”（麦克风） |
| 画面     | 打开 Windows 相机 / OBS / guvcview | 512×480、颜色正常、无红/绿鬼影（高对比边缘干净）              |
| 视频录制   | 相机 App 拍照 / OBS 录屏             | 能拍照、能录到 MJPEG 视频                                    |
| 声音     | 录音软件（语音录音机 / OBS 音频输入）         | 录到 NES 游戏音（非 1 kHz 测试音时）；或空闲时为 1 kHz 测试音            |
| 串口日志   | 115200 8N1                     | 见 6.4 的启动序列；无 `exception_entry` 崩溃                  |

> Windows 对每个 USB 设备会探测 MS OS 描述符（`GET_DESCRIPTOR string 0xEE`），我们没实现，会打印 `descriptor <type:3,index:ee> not found` —— **无害**，不影响枚举（系统按 IAD 自动加载 `usbvideo.sys` + `usbaudio.sys`）。

---

## 8. 内存布局

所有大缓冲都放在 PSRAM（`.psram_noinit` 段，`NES_PSRAM_BSS` 宏），SRAM 留给栈 + 少量静态量。

| 缓冲 | 大小 | 位置 | 说明 |
| --- | --- | --- | --- |
| `g_nes_rom[]` | 2 MB（`NES_ROM_MAX_SIZE`） | PSRAM | 启动从 flash `0x100000` 读入的 `.nes` 镜像 |
| `yuyv_buf[]` | 491 KB（512×480×2） | PSRAM | HW MJPEG 的 YUYV 输入；2× 关时仅 120 KB |
| `jpeg_buf[]` | 256 KB（`MJPEG_BUF_SZ`，2× 开） | PSRAM | 单帧 MJPEG 输出；2× 关时 80 KB |
| `nes_audio_ring[]` | ~22 KB | PSRAM | APU→UAC 的 16-bit 立体声环形缓冲 |
| InfoNES 核心（`WorkFrame`/`ChrBuf`/`PPURAM`/`RAM`/`SRAM`） | ~184 KB | PSRAM（链接入 `ram_psram` 区） | 模拟器内部状态，固定占用 |

> 链接阶段 `ram_psram` 区使用率约 **95%**（余量紧张）。新增 PSRAM 大缓冲（如 §10.4 的 RGB565→YUV LUT ~192 KB）前，务必确认链接不 overflow，或先释放其他缓冲。

---

## 9. 调优与已知问题

### 9.1 高对比色边（chroma fringing）

根因：**Windows `usbvideo.sys` 把 MJPEG 解压后强制转成 4:2:2 YUY2 显示**（水平方向色度砍半）。当 MJPEG 是 256×240 真 4:4:4 时，主机在偶数列重新采样色度，高对比彩色边缘（如 CONTRA 标题字）就会出现红/绿鬼影——这是主机端固有限制。

**为什么现在默认 512×480 就没色边了**：见 §4.2 的 2× 色度对齐数学。因为 DCT/量化/哈夫曼是硬件做，2× 几乎不增加 CPU 成本（串口 `[NES] enc fps=` 仍很高，见 §10）。

> 若确实需要 256×240（省带宽/兼容性），把 `uvc_uac.h` 的 `NES_FRAME_UPSCALE_2X` 改 `0` 重新编译即可；代价是高对比边缘会重新出现色边（主机 4:2:2 限制），这是已知权衡，不是 bug。

### 9.2 FPS 计数日志的标签（已知小瑕疵）

`InfoNES_LoadFrame()` 的 FPS 计数行打印的是 **`NES_DISP_WIDTH/HEIGHT`（256×240，NES 源分辨率）**，不是实际流出的 UVC 分辨率：

```
[NES] enc fps=19 (256x240 q90)     ← 这里 256x240 是“源”，实际 UVC 流是 512x480
```

这是**纯日志标签问题**，不影响功能（真实分辨率由 `VIDEO_WIDTH/HEIGHT` 决定，UVC 描述符与 JPEG SOF0 都一致）。下一轮优化（§10.4）计划把该行改为打印真实输出分辨率，避免误读。

### 9.3 音频音量 / 失真

- `NES_AUDIO_GAIN`（默认 40）：按耳朵调。偏小就 ×2 往上试，偏大/破音就 ÷2。已带去直流（DC blocker），输出是正确双极性。
- 若游戏无声：先确认听到的是不是 1 kHz 测试音（说明 NES 没在跑 / 没加载 ROM）。串口应有 `[NES] Main start`。
- 偶发爆音：NES 产出速度与 UAC 消费速度瞬时不匹配导致环形缓冲上溢/下溢，属已知 minor，不影响功能。

### 9.4 颜色

- 若整体偏暗：确认主机是按 limited range 解码（UVC 惯例）。极少数 MJPEG 图像查看器按 full-range 解码会偏暗。
- 个别颜色怪：优先查 InfoNES 对 CHR=0（CHR RAM）的处理。

---

## 10. 性能优化记录

### 10.1 瓶颈定位方法

`enc fps` 计数的是 `InfoNES_LoadFrame()` 成功编码的次数，而编码受 `jpeg_state` 门控（USB 取走才复位）→ **enc fps = min(编码速率, USB 消费速率)**。因此：

- 若 `enc fps` 远小于描述符声明值（15）→ 瓶颈在**编码侧**（模拟+转换+HW编码+dcache）。
- 若 `enc fps` ≥ 描述符声明值 → USB 没在卡描述符，瓶颈仍在**单帧 `T_enc`**（编码侧），只是 USB 消费更快。

实测 `enc fps=19~20` **超过**描述符 15 → 说明瓶颈是单帧 `T_enc ≈ 50ms`（NES 模拟 6502+PPU + RGB→YUYV + HW 编码 + dcache 维护），不在 USB/带宽。

### 10.2 已完成：软件 MJPEG → 硬件 MJPEG

- 初版用纯软件定点 `fdct` 编码器（`soft_mjpeg.c`），256×240 下也偏慢，且 512×480 不可行。
- 改用 BL616 硬件 MJPEG（`hw_mjpeg.c` 封装 `bflb_mjpeg_*`）：DCT/量化/哈夫曼全硬件。
- 结果：512×480 @q90 下 `enc fps≈10`（软→硬后编码本身不再是瓶颈，瓶颈转为模拟+转换）。

### 10.3 已完成：② 去冗余转换 + ① 热点 -O2

| 优化 | 内容 | 效果 |
| --- | --- | --- |
| ② 去冗余 2× 转换 | `hw_mjpeg_encode` 的 2× 路径改为按 NES 像素遍历，每像素算一次 `(Y,Cb,Cr)` 填 2×2 块（原循环每 NES 像素被转换 4 次且做无效平均） | `rgb565_ycbcr` 调用 245760 → 61440（÷4） |
| ① 热点 -O2 | 对 K6502/InfoNES/InfoNES_Mapper/InfoNES_pAPU/infones_port/hw_mjpeg/frame_audio_source 设 `COMPILE_FLAGS "-O2"`，覆盖工程默认 `-Os` | 6502 解释器 + PPU + 逐像素运算提速 |

**结果**：`enc fps` 从 **10 → 19~20**（512×480 @q90）。编译通过（`Built target combine`，`ram_psram` 约 95% 无 overflow），已烧录 COM6（SHA 校验通过，ROM 分区 `0x100000` 未动）。

### 10.4 待实施：③ 候选（已分析，未动手）

下一轮从编码侧进一步压 `T_enc`，按性价比排序：

| # | 手段 | 作用位置 | 预期影响 | 风险/代价 |
| --- | --- | --- | --- | --- |
| A | **RGB565→YUV 查表 LUT** | 编码侧逐像素转换 | 61440 次/帧的乘加移位 → 1 次查表（预建 65536 项 (Y,Cb,Cr) ≈ 192 KB，放 PSRAM）。编码侧**最大单点收益** | 多占 192 KB PSRAM（余量紧张，需确认） |
| B | **描述符 `VIDEO_FPS` 15→30** | USB 协商 | 让主机能协商更高帧率；当前实测已超 15，提 30 可能释放更高 fps（若编码跟得上） | 极低；bitrate 声明随之翻倍（HS 带宽绰绰有余） |
| C | **yuyv 缓冲改 non-cacheable** | dcache 维护 | 免去每帧 491 KB `dcache_clean_range`（纯开销） | 中：CPU 写非缓存 PSRAM 更慢，需实测权衡 |
| D | **热点 `-O3`**（替换 `-O2`） | NES 模拟 | 6502 解释器可能再快一截 | 低；体积略增 |
| E | **先加 encode 计时打点** | 诊断 | 在 `hw_mjpeg_encode` 前后用 tick 打点，串口打印每帧编码 ms，先定位 encode vs emulate 各占多少 | 零风险，纯观测 |

> 注：FPS 计数行的 `256×240` 标签问题（§9.2）也会随本轮修正。

---

## 11. 换游戏 ROM（只烧 ROM 分区，免重编固件）

固件与游戏 ROM 是**两个独立 Flash 分区**：

| 分区 | 偏移 | 内容 | 烧录方式 |
| --- | --- | --- | --- |
| 固件 | `0x000000` | UVC+UAC+NES 模拟器（不含任何 ROM） | `flash_prog_cfg_com6.ini` / 见 §6.3 |
| ROM  | `0x100000` | 一个原始 `.nes` 镜像（即烧即读） | `tools/flash_rom.py` / 见下 |

启动时 `nes_bridge_init()` 用 `bflb_flash_read()` 把 ROM 分区整段读进 PSRAM 缓冲 `g_nes_rom[]`，`InfoNES_ReadRom` 直接解析，**无需重编固件**。ROM 缓冲上限 `NES_ROM_MAX_SIZE = 2 MB`；超 2 MB 会截断并串口告警。

换游戏步骤：

1. 准备 `.nes` 文件（NROM/mapper0 最省事，如早期 SMB）。
2. 解析 iNES 头确认 mapper 号（**mapper 决定固件需编进对应 Mapper，不只是换 ROM**）：
   ```bash
   python -c "import sys;d=open('game.nes','rb').read(16);print('PRG',d[4],'CHR',d[5],'mapper',(d[6]>>4)|(d[7]&0x0f))"
   ```
3. 若 mapper > 0 且当前固件未包含该 mapper（当前 `MAPPERS=[0,2]`：NROM + UxROM；魂斗罗等 mapper2 游戏已支持，无需重编）：
   - 编辑 `tools/gen_mapper.py` 的 `MAPPERS` 加上对应编号（每加一个 mapper 可能引入数 KB~256 KB SRAM 静态量，需确认 SRAM 余量）；
   - 重跑生成 `InfoNES_Mapper.c` → **重新编译并烧固件**（§6.3）。
   - 若 mapper 已在固件内，跳到下一步。
4. 只烧 ROM 分区（板子进下载模式，见 §6.2）：
   ```bash
   # 方式 A：脚本（推荐，自动校验 NES 头、生成临时配置）
   python tools/flash_rom.py C:/path/to/game.nes --port COM6
   # 方式 B：手动 ini
   #   改 flash_prog_cfg_rom.ini 的 filedir = 你的 game.nes 绝对路径
   #   然后：BLFlashCommand.exe --port COM6 --config flash_prog_cfg_rom.ini --chipname bl616 write_flash_files
   ```
   > 该步骤 `erase=1` **仅擦 ROM 区 0x100000 起的扇区，不会动 0x0 的固件**。固件只需烧一次，之后换游戏只跑这一步。
5. 松开 BOOT、按 RESET 启动，新游戏直接生效。串口应有：
   ```
   [NES] ROM loaded from flash @0x100000, len=<N> (PRG=.. CHR=.. trainer=..)
   [NES] Load ret=0
   [NES] Main start
   ```

无 ROM / ROM 损坏时：串口打印 `[NES] no valid ROM in flash @0x100000 (expected 'NES\x1a')`，NES 空闲、UVC 走彩条测试图案（不黑屏），用本节的 ROM 烧录步骤烧一个即可。

> 内置 demo（`tools/make_demo_rom.py` 生成）是整屏纯色，用于 smoke-test（全红代表 PPU→MJPEG→UVC 链路已通），不是真实游戏。

---

## 12. 手柄输入（HID OUT，已实现）

输入钩子已就位：`infones_port.c` 里有 `nes_bridge_set_pad(uint32_t)` 与 `pad_state`，
`InfoNES_PadState()` 每帧从 `pad_state` 取 PAD1。任何数据源只要算出 NES 位掩码并调用
`nes_bridge_set_pad()` 即可（包括 GPIO、BLE、串口——当前实现的是 USB HID）。

**位掩码（与 InfoNES PAD1 一致，硬件侧 `infones_port.c:144`）：**

| 位 | b7 | b6 | b5 | b4 | b3 | b2 | b1 | b0 |
|----|----|----|----|----|----|----|----|----|
| 键 | Right | Left | Down | Up | Start | Select | B | A |

各键 1 字节值：`A=0x01 B=0x02 Select=0x04 Start=0x08 Up=0x10 Down=0x20 Left=0x40 Right=0x80`。

**USB HID OUT 方案（零额外硬件，复用已插的 USB 线）：**

- BL616 现在是插在 PC 上的 USB **设备**（UVC 摄像头 + UAC 声卡），同一路 USB
  **不能同时当 host 接有线手柄**。所以走"PC 读真手柄 → 通过现有 USB 线写回 NES 按钮"的路子。
- 在 UVC+UAC 复合设备里新增了 **第 4 个接口（iface 4）= HID**，带一个 **中断 OUT 端点 0x03**
  （见 `usb_composite.c`）。HID 报告描述符：`1 字节 Output 报告，Report ID=1，vendor usage page 0xFF00`。
- 双通道收数据（任一均可）：
  1. **OUT 端点 0x03**：PC 每次写 `bytes([1, nes_pad_byte])`（report id + 1 字节）→ 固件
     `hid_out_ep_cb` 取 `buf[1]` 调 `nes_bridge_set_pad()`，并 `usbd_ep_start_read` 重触发。
  2. **Set_Report 控制通道（EP0）**：`usbd_hid_set_report()` 被 override，直接取 `report[0]` 喂
     `nes_bridge_set_pad()`。作为无 OUT 端点发送器的兜底。
- `defconfig` 已开 `CONFIG_CHERRYUSB_DEVICE_HID =y`（libcherryusb 才编入 `usbd_hid.c`）。

**PC 发送工具（`tools/`，任选其一）：**

- `tools/nes_pad_sender.py`（推荐，需 `pip install hid pygame`）：
  `python tools/nes_pad_sender.py` 自动找 VID/PID `ABCD:1234` 的 HID；
  `--keyboard` 用 WASD+JKUI；`--list` 列设备；`--vid/--pid` 自定义。
  手柄映射：方向键/左摇杆=方向，A(0)=A、B(1)=B、Back(6)=Select、Start(7)=Start。
- `tools/nes_pad_webhid.html`（零安装，Chrome）：经 `localhost` 或 `https` 打开（file:// 不可用
  WebHID），点"连接设备"选 `ABCD:1234`，键盘/手柄经 Gamepad API 转发。

**验证**：插 USB 后设备管理器应多出一个 HID 设备（无报错）；运行发送工具，Contra 标题屏可用
A/B/方向操作，串口无 USB 枚举错误（`[UVCA] hid intf4 ... ok`）。

---

## 13. 故障排查速查

| 现象                                      | 可能原因                                   | 处理                                                          |
| --------------------------------------- | -------------------------------------- | ----------------------------------------------------------- |
| 上电即 `exception_entry mcause=0x38000003` | `bflb_device_get_by_name("usb")` 找不到设备 | 必须用 `BFLB_NAME_USB_V2`（即 `"usb_v2"`）                        |
| `IMG LOAD HANDSHAKE FAIL`               | 板子没进下载模式（在跑旧固件）                        | 按 6.2 手动 BOOT+RESET 进 ISP                                   |
| 摄像头能开但黑屏/不能拍照                           | 帧源没接（NES 关时未兜底）                        | 已接 soft_mjpeg 测试图案兜底；确认 `video_source_get_frame` 走 fallback |
| 颜色不对/不同颜色偏移不同                           | MJPEG range 不匹配                        | 已改 studio-range；仍偏暗查主机解码                                    |
| 文字边缘虚                                   | quality 低 / 主机放大                       | quality 已 90；可选 2× 放大（见 4.2）                                |
| 红/绿鬼影                                   | 关了 2× 放大（256×240）                     | 确认 `NES_FRAME_UPSCALE_2X=1`（默认开）                           |
| 音频刺耳/失真                                 | GAIN 过高 + 无去直流                         | 已改 DC blocker + `NES_AUDIO_GAIN=40`                         |
| 烧录 COM 被占                               | 串口监视器占用                                | 关闭占用 COM6 的终端再烧                                             |

---

## 14. 工程约定

- **只改工程 app 代码，不碰 SDK**：`main.c` / `usb_composite.c` / `frame_audio_source.c` / `nes/` 下的文件都是工程自有；SDK 的路径、CherryUSB 类驱动、链接脚本均不动。
- **大缓冲进 PSRAM**：用 `.psram_noinit` 段（`NES_PSRAM_BSS` 宏），并在 `nes_bridge_init()` 里显式 `memset` 清零（NOLOAD 段不保证启动清零）。
- **分层日志**：`[UVCA]`（USB 复合设备）、`[NES]` / `[NESPRT]`（InfoNES 桥接）、`[FRAME]`（帧/音频源），便于定位死在哪个阶段。
- **性能优化走增量**：一次一个瓶颈（②+① → ③），每步编译+烧录+串口 `[NES] enc fps=` 对比，先用数据定位再动手（见 §10）。
