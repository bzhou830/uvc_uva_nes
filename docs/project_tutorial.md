# BL616 UVC + UAC + NES 模拟器串流项目 — 从原理到实现的完整教程

> 本文档把 `uvc_uac_nes` 工程**从硬件原理、系统架构、构建系统、USB 复合设备、视频管线（含色彩数学与去鬼影）、音频管线、ROM 加载、InfoNES 移植、性能优化、到 HID 手柄输入**全部串成一份端到端教程。所有行号、宏、字节、位掩码均取自当前真实源码（2026-08-28 状态）。
>
> 目标读者：已熟悉 C / 嵌入式 / USB 基础概念，想**逐技术细节**理解这个项目怎么搭起来的开发者。

---

## 目录

1. 项目目标与一句话原理
2. 硬件平台（BL616 关键特性）
3. 系统架构与数据流
4. 工程结构与构建系统
5. USB 复合设备原理（UVC + UAC + HID）
6. 视频管线：NES PPU → MJPEG → UVC
7. 音频管线：NES APU → PCM → UAC
8. ROM 加载与内存布局
9. InfoNES 移植（可移植 API 与桥接）
10. 性能优化（瓶颈定位、已做、待做）
11. HID OUT 手柄输入子系统
12. 烧录 / 测试 / 排错速查
13. 附录（常量表、最小复现清单）

---

## 1. 项目目标与一句话原理

把开源 NES 模拟器 **InfoNES** 搬到 **BL616（RISC-V 320 MHz，无 FPU）** 上跑；每帧画面经 **BL616 内置硬件 MJPEG 编码器** 压成 JPEG，通过 **UVC（USB 摄像头类）** 传到 PC；NES APU 声音经 **UAC（USB 麦克风类）** 传出来；再额外加一个 **HID OUT 接口** 收 PC 转发的手柄按键。PC 上打开"相机"就能看画面、打开"录音机"就能录声音、敲键盘就能玩。

**一句话原理**：板子是插在 PC 上的 USB **设备**，它同时扮演"摄像头 + 声卡 + HID 接收器"三种角色（IAD 复合设备）；NES 游戏在板子本地跑，画面/声音实时编码后通过 USB 流出，手柄按键则通过同一条 USB 线从 PC 写回板子。

---

## 2. 硬件平台（BL616 关键特性）

| 项目 | 说明 | 对工程的影响 |
| --- | --- | --- |
| 芯片 | Bouffalo Lab **BL616**（RISC-V，320 MHz，**无 FPU**） | 浮点贵 → 全程整数运算；APU/色彩转换都用定点 |
| 内存 | 内部 SRAM ~**415 KB**；板载 **4 MB PSRAM**（`0xA8000000` 起） | 所有大缓冲进 PSRAM（`.psram_noinit`），SRAM 留给栈/少量静态量 |
| USB | 内置 USB 2.0 **Device**（Full-Speed@12Mbps 或 High-Speed@480Mbps，取决于 `CONFIG_USB_HS`） | 只能当 Device，不能当 Host 接手柄 → 手柄必须走 HID OUT 由 PC 转发 |
| 硬件 MJPEG | 芯片内独立 MJPEG 块 | DCT/量化/哈夫曼全硬件做；软件只做 RGB565→YUYV（很便宜） |
| 串口 | 板载 USB-SERIAL（CH340），烧录口 **COM6** | 日志 115200 8N1；烧录用 BLFlashCommand |

> **为什么不需要 FPU 也不慢**：MJPEG 的 DCT/量化/哈夫曼由硬件 MJPEG 块完成；软件侧只剩 RGB565→YUYV 的整数移位乘加、NES 6502 解释器、PPU 渲染。后两者 ALU/分支密集，用 `-O2`（见 §4.3）即可跑满。

---

## 3. 系统架构与数据流

```
                ┌──────────────────────────── BL616 (bl616dk) ────────────────────────────┐
                │                                                                          │
  NES ROM ──► InfoNES 核心 ──┬──► PPU ──► WorkFrame(RGB565 256×240)                        │
  (flash      (K6502+APU)    │                                                              │
   0x100000)                  │   hw_mjpeg_encode (HW MJPEG, 软件做 RGB565→YUYV 4:2:2)     │
                              │   2× 放大在此顺带完成（每 NES 像素→2×2 YUYV 块）            │
                              │            │  MJPEG                                        │
                              │            ▼                                                │
                              │  jpeg_buf(PSRAM)  ← jpeg_state 门控（单缓冲）              │
                              │            │                                                │
                              │            ▼                                                │
                              │      UVC 视频流 (ep 0x81, iso IN) ─────────┐               │
                              │                                           │  USB ─► 主机相机  │
                              └──► APU ──► InfoNES_SoundOutput ──────────┐│                │
                                  5 路波形 → DC blocker → 混音 → 削波     │▼                │
                                  │ int16 立体声                           nes_audio_ring   │
                                  ▼                            (PSRAM 环形)                  │
                          UAC 麦克风流 (ep 0x82, iso IN) ───────────────┘  USB ─► 主机录音  │
                                                                                                │
   HID OUT (ep 0x03) ◄──────── PC 转发 NES 按键（w/a/s/d/j/k/Enter/Space 或真手柄）◄───────┘
                └──────────────────────────────────────────────────────────────────────────┘
```

**两个 RTOS 任务**（见 `main.c`）：

| 任务 | 优先级 | 职责 |
| --- | --- | --- |
| `uvc_uac_task` | 5（高） | 初始化 USB 复合设备、`uvc_uac_poll()` 轮询、调度 UVC/UAC 端点传输（不被饿死） |
| `nes_task` | 4（低） | `InfoNES_Main()` 模拟循环：每帧 PPU 渲染 → `InfoNES_LoadFrame()` 编码 → APU 混音 → `InfoNES_Wait()` 让出 16ms |

**视频单缓冲 + 状态门控**（BL616 单核，状态标志即可避免竞争，无需互斥锁）：

```
nes_task:  InfoNES_LoadFrame()   if(jpeg_state==0){ 编码→jpeg_buf; jpeg_state=1 }
uvc_task:  nes_bridge_get_video() if(jpeg_state==1){ 取走; jpeg_state=2 }
uvc_iso:  传输完成回调 nes_bridge_release_video() if(jpeg_state==2){ jpeg_state=0 }
```

编码与 USB 传输**严格串行**：某帧没被 USB 取走前，下一帧不重编码。这条性质是性能分析的核心（见 §10）。

---

## 4. 工程结构与构建系统

### 4.1 目录结构

```
uvc_uac_nes/
├── main.c                  # 建 uvc_uac_task + nes_task，调 uvc_uac_init / poll
├── usb_composite.c         # UVC+UAC+HID 描述符、端点、轮询、源回调
├── uvc_uac.h               # 接口/端点/分辨率/采样率常量 + 源 API 声明
├── frame_audio_source.c    # 实现 video_source_get_frame / audio_source_fill
├── defconfig               # SDK 配置开关
├── CMakeLists.txt          # InfoNES 各源文件 + 热点 -O2
├── flash_prog_cfg*.ini     # 固件 / ROM 烧录配置
├── nes/
│   ├── hw_mjpeg.c/.h       # 硬件 MJPEG 封装（RGB565→YUYV→HW encode）
│   ├── jpeg_head.c/.h      # JPEG 头生成（DQT/SOF/SOS）
│   ├── soft_mjpeg.c/.h     # [兜底] 软件定点 baseline JPEG（测试图案 fallback）
│   ├── infones_port.c/.h   # InfoNES 可移植 API + NES↔USB 桥接
│   └── infones_core/       # InfoNES 核心（InfoNES.c / K6502.c / InfoNES_Mapper.c / InfoNES_pAPU.c / 调色板）
└── tools/
    ├── nes_pad_sender.py   # PC 键盘/手柄 → HID OUT
    ├── nes_pad_webhid.html # Chrome WebHID 转发
    ├── flash_rom.py        # 烧 .nes 到 ROM 分区
    ├── gen_mapper.py       # 生成 InfoNES_Mapper.c
    └── （辅助脚本若干）
```

### 4.2 `CMakeLists.txt` 逐行要点（`CMakeLists.txt`）

```cmake
cmake_minimum_required(VERSION 3.15)
find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})   # 找 SDK（BL_SDK_BASE 必须 Windows 路径）
sdk_add_compile_definitions(-D SHELL_THREAD_PRIO=20)
sdk_add_include_directories(.)        # 工程根目录（让 nes/ 子目录可被 #include "nes/xxx.h"）
sdk_set_main_file(main.c)             # 入口是 main.c

target_sources(app PRIVATE
    ./usb_composite.c
    ./frame_audio_source.c
    ./nes/infones_port.c
    ./nes/soft_mjpeg.c          # Phase-1 测试图案编码器（无 ROM 时兜底）
    ./nes/hw_mjpeg.c            # 硬件 MJPEG 编码器（NES 帧）
    ./nes/jpeg_head.c           # JPEG 头
    ./nes/infones_core/InfoNES.c
    ./nes/infones_core/InfoNES_Mapper.c
    ./nes/infones_core/InfoNES_pAPU.c
    ./nes/infones_core/K6502.c
)
```

**关键坑 1**：`InfoNES_Mapper.c` 内部 `#include` 了 `nes/infones_core/mapper/*.c` 的所有 mapper，所以**不要**在 `target_sources` 里再列 mapper 文件，否则链接期符号重复。

**关键坑 2（性能）**：SDK 默认 `-Os`（体积优先）。InfoNES 的 6502 解释器 + PPU 渲染、逐像素 RGB565→YUYV 是 ALU/分支密集代码，在 `-Os` 下偏慢。对热点翻译单元单独加 `-O2`：

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

> **为什么 `-O2` 能覆盖 `-Os`？** CMake 生成命令行时，per-file 的 `COMPILE_FLAGS` 出现在**全局 `CMAKE_C_FLAGS`（含 `-Os`）之后**；GCC 后面的 `-O` 等级生效，所以这些 TU 实际以 `-O2` 编译。验证：`build/CMakeFiles/app.dir/build.make` 里这些 `.c.obj` 命令行含 `-O2`，其余仍 `-Os`。

### 4.3 `defconfig` 关键开关

```
CONFIG_PSRAM = y                 # 4MB PSRAM 初始化 + 堆注册（所有大缓冲依赖它）
CONFIG_COREDUMP = n              # 关崩溃时 base64 coredump 刷屏（看干净日志）
CONFIG_CHERRYUSB_DEVICE_VIDEO = y  # 编入 UVC 类驱动
CONFIG_CHERRYUSB_DEVICE_AUDIO  = y  # 编入 UAC 类驱动
CONFIG_CHERRYUSB_DEVICE_HID   = y  # 编入 usbd_hid.c（手柄接收器依赖它！漏了 HID 接口就不工作）
```

> 改 `defconfig` 后 SDK 的 `make` 会自动重读注入，**无需 `rm -rf build`**。

### 4.4 构建命令

```bash
cd D:/bouffalo_sdk/examples/cherryusb/uvc_uac_nes
export BL_SDK_BASE=D:/bouffalo_sdk CHIP=bl616 BOARD=bl616dk   # 注意 BL_SDK_BASE 用 Windows 风格 D:/...
make
```

> **路径坑**：SDK 自带 mingw `make` 不翻译 `/d/...`，`BL_SDK_BASE` 必须写 **`D:/bouffalo_sdk`**（盘符冒号），否则找不到 SDK。

产物（`build/build_out/`）：`cherryusb_bl616.bin`（要烧的主固件）、`.elf`、`.map`。`.xz` 报 "ota exceeds partition" 是**无害**告警，不影响要烧的 `.bin`。

---

## 5. USB 复合设备原理（UVC + UAC + HID）

### 5.1 复合设备与 IAD

一个 USB 设备挂多个功能类，用 **IAD（Interface Association Descriptor）** 把若干接口归到一个功能。本设备在**设备描述符**里声明 `bDeviceClass=0xEF / SubClass=0x02 / Protocol=0x01`（"Miscellaneous / 复合设备"），让 Windows 自动按 IAD 加载对应驱动（`usbvideo.sys` + `usbaudio.sys` + HID）。

设备描述符（`usb_composite.c:98`）：
```c
static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xef, 0x02, 0x01, USBD_VID, USBD_PID, 0x0001, 0x01)
};
// USBD_VID=0xABCD  USBD_PID=0x1234  （主机上看到的相机/声卡/ HID 的 VID/PID 即 ABCD:1234）
```

接口/端点分配（`uvc_uac.h:42-48`）：

| 接口 | 功能 | 端点 |
| --- | --- | --- |
| iface 0 | UVC VideoControl (VC) | — |
| iface 1 | UVC VideoStream (VS) | **0x81** iso IN |
| iface 2 | UAC AudioControl (AC) | — |
| iface 3 | UAC AudioStream (mic) | **0x82** iso IN |
| iface 4 | HID (手柄接收) | **0x03** interrupt OUT |

> **端点地址方向位**：`0x80` 是方向位（IN = 设备→主机）。所以 `0x81`/`0x82` 是 IN，`0x03` 是 OUT（手柄按键从 PC→板子，必须用 OUT）。

### 5.2 CherryUSB 的三层注册

CherryUSB 把"描述符 / 接口 / 端点"三者分开注册（`uvc_uac_init`，`usb_composite.c:291`）：

```c
usbd_desc_register(busid, &composite_descriptor);                 // 1. 描述符回调（设备/配置/字符串）
usbd_add_interface(busid, usbd_video_init_intf(... &video_intf0)); // 2a. VC
usbd_add_interface(busid, usbd_video_init_intf(... &video_intf1)); // 2b. VS
usbd_add_interface(busid, usbd_audio_init_intf(... &audio_intf0)); // 2c. AC
usbd_add_interface(busid, usbd_audio_init_intf(... &audio_intf1)); // 2d. AS mic
usbd_add_interface(busid, usbd_hid_init_intf(... &hid_intf, ...)); // 2e. HID
usbd_add_endpoint(busid, &video_in_ep);   // 3a. 0x81
usbd_add_endpoint(busid, &audio_in_ep);   // 3b. 0x82
usbd_add_endpoint(busid, &hid_out_ep);    // 3c. 0x03
usbd_initialize(busid, reg_base, usbd_event_handler);
```

**注册顺序必须和描述符里的接口顺序一致**（VC0, VS1, AC2, AS3, HID4），否则接口号错位。

### 5.3 视频/音频源回调与轮询（`uvc_uac_poll`，`usb_composite.c:324`）

USB 是事件/轮询驱动。RTOS 里 `uvc_uac_task` 每 1ms 调一次 `uvc_uac_poll()`：

```c
void uvc_uac_poll(uint8_t busid) {
    if (video_streaming && !video_busy) {            // 上一帧传完才发下一帧
        const uint8_t *frame; uint32_t len;
        if (video_source_get_frame(&frame, &len) == 0 && len > 0) {
            video_busy = true;
            usbd_video_stream_start_write(busid, VIDEO_IN_EP, video_packet_buf,
                                          (uint8_t *)frame, len, true);  // 拆包成 iso 微帧
        }
    }
    if (audio_streaming && !audio_busy) {
        audio_source_fill(audio_write_buf, AUDIO_IN_PACKET);   // 取 NES APU PCM 或 1kHz 兜底
        audio_busy = true;
        usbd_ep_start_write(busid, AUDIO_MIC_EP, audio_write_buf, AUDIO_IN_PACKET);
    }
}
```

- `video_source_get_frame` / `audio_source_fill` 是**可插拔源 API**（在 `uvc_uac.h` 声明，由 `frame_audio_source.c` 实现）。
- NES 在跑 → 取 `nes_bridge_get_video()`（MJPEG）/ `nes_bridge_get_audio()`（PCM）；没跑 → 走测试图案/1kHz 兜底（§6.5 / §7）。
- 当一帧 iso 传完，`usbd_video_iso_callback`（`usb_composite.c:210`）调 `nes_bridge_release_video()` 把 `jpeg_state` 复位为 0，允许下一帧重编码。

### 5.4 UVC 描述符要点

UVC 模板自带描述符（VC 接口、VS 接口、MJPEG 格式、帧描述符）。本项目重编号接口/端点拼成复合配置：

```c
VIDEO_VS_FRAME_MJPEG_DESCRIPTOR_INIT(0x01, VIDEO_WIDTH, VIDEO_HEIGHT,
    VIDEO_MIN_BIT_RATE, VIDEO_MAX_BIT_RATE, VIDEO_MAX_FRAME_SIZE,
    DBVAL(VIDEO_INTERVAL_NS), 0x01, DBVAL(VIDEO_INTERVAL_NS)),
// VIDEO_WIDTH/HEIGHT = 512×480（2× 开），VIDEO_FPS=15，VIDEO_INTERVAL_NS=10^7/15 (100ns 单位)
```

描述符里的帧宽高、JPEG `SOF0` 头里的宽高、实际 YUYV 缓冲宽高三者**必须一致**（本项目都由 `VIDEO_WIDTH/HEIGHT` 派生），否则主机解码错位。

---

## 6. 视频管线：NES PPU → MJPEG → UVC

### 6.1 总体流程

1. InfoNES 每渲染完一帧，按 `NesPalette[]`（RGB565）写入 `WorkFrame[256×240]`（`infones_port.c` 里 InfoNES 核心的全局数组）。
2. `InfoNES_LoadFrame()`（`infones_port.c:273`）把 `WorkFrame` 交给 `hw_mjpeg_encode()` 编码成 MJPEG，写 `jpeg_buf`，置 `jpeg_state=1`。
3. `uvc_uac_poll()` 取帧并启动 `usbd_video_stream_start_write()`。
4. iso 传完回调 `nes_bridge_release_video()` 复位 `jpeg_state=0`。

### 6.2 BT.601 色彩数学（逐公式）

RGB565→YUYV 在 `hw_mjpeg.c:rgb565_ycbcr`（整数、BT.601 studio / limited range）：

```c
static inline void rgb565_ycbcr(uint16_t p, int *y, int *cb, int *cr) {
    int r = (p >> 11) & 0x1F; r = (r << 3) | (r >> 2);   // 5→8 bit 扩展（复制高位）
    int g = (p >> 5)  & 0x3F; g = (g << 2) | (g >> 4);   // 6→8 bit
    int b = p         & 0x1F; b = (b << 3) | (b >> 2);   // 5→8 bit
    *y  = 16  + (( 66 * r + 129 * g +  25 * b) >> 8);
    *cb = 128 + ((-38 * r -  74 * g + 112 * b) >> 8);
    *cr = 128 + ((112 * r -  94 * g -  18 * b) >> 8);
}
```

系数来源：标准 BT.601（Rec.601）常量
- `Y  = 0.257R + 0.504G + 0.098B`
- `Cb = -0.148R - 0.291G + 0.439B`
- `Cr = 0.439R - 0.368G - 0.071B`

乘以 256 取整得到 `{66,129,25,-38,-74,112,112,-94,-18}`。注意 **`+16` / `+128`**：这是 **studio / limited range**（Y∈[16,235]，Cb/Cr∈[16,240]）。

> **为什么必须用 limited range？** UVC/MJPEG 在 Windows 上由 `usbvideo.sys` 按 limited range 解码。若用 full range（Y∈[0,255]），宿主会再拉伸一次 → 暗部压暗、低/中亮度颜色整体偏移（"不同颜色偏的程度不一样"的典型症状）。本项目已用 studio range 修正。

### 6.3 YUYV 4:2:2 打包

YUYV 每**两个像素共享一组 Cb/Cr**（色度水平半采样）：

```
字节序：  [Y0][Cb][Y1][Cr]   [Y2][Cb][Y3][Cr]  ...
                ↑ Cb/Cr = 像素 0、1 的居中平均
```

### 6.4 2× 上采样与去鬼影数学（核心）

**问题**：Windows `usbvideo.sys` 把 MJPEG 解码成 4:2:2 YUY2 后做**共址（cosited）色度**——每个 Cb/Cr 归属"偶数列像素"，奇数列由相邻偶数插值。若直接传 256×240 真 4:4:4，高对比彩色边缘（如 CONTRA 标题字红边挨绿边）会出现**红/绿鬼影**（这是主机端固有限制，纯软件 256×240 无法消除）。

**解法**：默认开 `NES_FRAME_UPSCALE_2X`（`uvc_uac.h:59`，=1），把每个 NES 像素展成 2×2 YUYV 块，且色度取该 NES 像素自身：

```
NES 像素 P ──► 输出 2×2 块（每行两像素都是 P 的拷贝）：
  第 2ny   行: [Y][Cb][Y][Cr]    ← Cb/Cr = P 自身色度
  第 2ny+1 行: [Y][Cb][Y][Cr]
```

因为 2×2 块里两个相邻输出像素是**同一 NES 像素的拷贝**，它们的"居中平均"色度 = P 自身色度（恒等）。于是主机在偶数列采到的色度**正好是该 NES 像素的正确色度**，奇数列插值也落在同一 NES 像素 → **红/绿鬼影消失、边缘锐利**。

**去冗余写法（性能优化 ②，`hw_mjpeg.c:122`）**：直接按 NES 像素遍历，每像素只算一次 `(Y,Cb,Cr)` 填 2×2 块（见下），比"按输出像素遍历、每对重复算 4 次"少 4× 的 `rgb565_ycbcr` 调用（245760→61440）：

```c
if (scale == 2) {
    for (int ny = 0; ny < h; ny++) {
        const uint16_t *row = rgb565 + (size_t)ny * w;
        uint8_t *o0 = yuyv + (size_t)(ny*2)   * ow * 2;  // 偶数行
        uint8_t *o1 = yuyv + (size_t)(ny*2+1) * ow * 2;  // 奇数行
        for (int nx = 0; nx < w; nx++) {
            int Y, Cb, Cr;
            rgb565_ycbcr(row[nx], &Y, &Cb, &Cr);
            int k = nx * 4;                               // 2 输出像素 × 2 字节
            o0[k]=Y; o0[k+1]=Cb; o0[k+2]=Y; o0[k+3]=Cr;   // 偶数行 2×2
            o1[k]=Y; o1[k+1]=Cb; o1[k+2]=Y; o1[k+3]=Cr;   // 奇数行（同值）
        }
    }
} else {  // 1×：标准 4:2:2，相邻两 NES 像素色度居中平均
    for (int y = 0; y < oh; y++) {
        ...
        for (int x = 0; x < ow; x += 2) {
            rgb565_ycbcr(row[x],  &Y0,&Cb0,&Cr0);
            rgb565_ycbcr(row[sx1],&Y1,&Cb1,&Cr1);
            o[0]=Y0; o[1]=(Cb0+Cb1)>>1; o[2]=Y1; o[3]=(Cr0+Cr1)>>1;
        }
    }
}
```

> 因为 DCT/量化/哈夫曼是硬件做，2× 几乎不增加 CPU 成本（实测 fps 不降）。关 2× 改 `NES_FRAME_UPSCALE_2X=0` 即回 256×240，代价是鬼影回归（已知权衡，非 bug）。

### 6.5 硬件 MJPEG 封装（逐步骤）

`hw_mjpeg_encode()`（`hw_mjpeg.c:97`）每帧：

```c
// 1. 上面已做的 RGB565→YUYV（写入 yuyv_buf，PSRAM）
// 2. 缓存一致性：YUYV 在 PSRAM（可缓存），HW 从它 DMA 读，先 clean
bflb_l1c_dcache_clean_range(yuyv, yuyv_sz);
// 3. 启动硬件编码（内存输入模式，无需 DVP 摄像头）
bflb_mjpeg_update_input_output_buff(mjpeg_dev, yuyv, NULL, out, out_size);
bflb_mjpeg_sw_run(mjpeg_dev, 1);
// 4. 轮询完成（单核，无别的活）
while (!(bflb_mjpeg_get_intstatus(mjpeg_dev) & MJPEG_INTSTS_ONE_FRAME)) {}
bflb_mjpeg_int_clear(mjpeg_dev, MJPEG_INTCLR_ONE_FRAME);
// 5. 取 JPEG（硬件自动加头 + EOI）
uint8_t *pic; uint32_t len = bflb_mjpeg_get_frame_info(mjpeg_dev, &pic);
bflb_mjpeg_pop_one_frame(mjpeg_dev); bflb_mjpeg_stop(mjpeg_dev);
// 6. 缓存一致性：HW 写回 JPEG 到 out，CPU 要先 invalidate
bflb_l1c_dcache_invalidate_range(out, len);
```

**缓存一致性（最易踩的坑）**：YUYV 输入缓冲在 PSRAM（可缓存），编码前 `dcache_clean_range()` 把 CPU 写的内容刷给 HW DMA；HW 写回 JPEG 后 `dcache_invalidate_range()` 让 CPU 看到新数据，否则读到陈旧内容。`jpeg_buf` 也在 PSRAM，所以第 6 步必做。

`hw_mjpeg_init()`（`hw_mjpeg.c:60`）配置一次：`format=MJPEG_FORMAT_YUV422_YUYV`、`quality=90`、输入输出缓冲地址、并用 `JpegHeadCreate(YUV_MODE_422, quality, w, h, ...)` 生成 DQT/SOF/SOS 头让硬件自动拼接。

> **quality=90**（`SOFT_MJPEG_QUALITY`，`infones_port.c:129`）：硬件量化表步进。太低（如初版 60）丢高频 → 文字发虚；90 保留文字边缘。512×480 @q90 单帧约 15–60 KB，`MJPEG_BUF_SZ=256KB` 足够。

### 6.6 兜底（无 ROM 时）

`video_source_get_frame`（`frame_audio_source.c:61`）：NES 没跑时，用软件编码器 `soft_mjpeg_encode()` 把一张彩条测试图案（`build_test_pattern`）编码成 MJPEG，**只编码一次**后反复流，保证 UVC 管道存活（相机能开、不黑屏）。

---

## 7. 音频管线：NES APU → PCM → UAC

### 7.1 APU 5 路波形与单极性 bug

`InfoNES_SoundOutput()`（`infones_port.c:324`）每 vsync 被调一次，参数是 5 路 APU 波形缓冲 `w1..w5`（2 脉冲 + 三角 + 噪声 + DPCM）。InfoNES 渲染出的是**单极性**信号（每个样本 ≥ 0，5 路求和约 0..9307），带巨大 DC 偏置、无负向摆幅。

**初版 bug**：直接 `GAIN=400` 乘 → 严重削波，且输出是"整流状"单极性波形 → 刺耳/失真。

### 7.2 修正：DC blocker + 增益 + 削波

```c
#define NES_AUDIO_GAIN      40
#define NES_AUDIO_DC_SHIFT  8    /* 一阶极点 1/256 ≈ 30 Hz 高通 */
static int dc_est = 0;
void InfoNES_SoundOutput(int samples, BYTE *w1..w5) {
    for (int i = 0; i < samples; i++) {
        int sum = (int)w1[i]+(int)w2[i]+(int)w3[i]+(int)w4[i]+(int)w5[i];
        int ac  = sum - dc_est;                  // 去 DC 偏置 → 正确双极性
        dc_est += (sum - dc_est) >> NES_AUDIO_DC_SHIFT;  // 一阶跟踪均值（极点~30Hz）
        int s = ac * NES_AUDIO_GAIN;
        s = clamp(s, -32768, 32767);            // 削波保护
        int16_t v = (int16_t)s;
        dst[audio_wr]=v; dst[audio_wr+1]=v;     // L/R 复制（NES 单声道）
        audio_wr = (audio_wr + AUDIO_CHANNELS) % AUDIO_RING_CAP;
    }
}
```

### 7.3 环形缓冲（SPSC）

`nes_audio_ring[]`（PSRAM，`infones_port.c:137`）是单生产者（NES 任务）/单消费者（USB 任务）环形缓冲，索引用 `int16` 单位。生产者 `InfoNES_SoundOutput` 写 `audio_wr`、消费者 `nes_bridge_get_audio`（`infones_port.c:215`）读 `audio_rd`，临界区用 `taskENTER_CRITICAL/EXIT_CRITICAL` 保护。空了就留静音（不爆音）。

### 7.4 采样率对齐

`InfoNES_SoundInit()`（`infones_port.c:382`）强制 `ApuQuality=2` → APU 44100 Hz、735 样本/同步。UAC 端点也是 `AUDIO_SAMPLE_RATE=44100`（`uvc_uac.h:73`），**无需重采样**。

### 7.5 1 kHz 兜底

NES 没跑时，`audio_source_fill()`（`frame_audio_source.c:113`）改发 1 kHz 正弦（`sine_lut[256]` 预生成），保证 UAC 麦克风一直有声（便于确认枚举/录音）。

---

## 8. ROM 加载与内存布局

### 8.1 Flash 双分区

| 分区 | 偏移 | 内容 | 烧录 |
| --- | --- | --- | --- |
| 固件 | `0x000000` | UVC+UAC+NES 模拟器（不含 ROM） | `flash_prog_cfg_com6.ini` |
| ROM | `0x100000` | ROM 分区：目录区(4KB) + 数据区（多 `.nes`，见 §14） | `tools/flash_rom.py` |

启动 `nes_bridge_init()`（`infones_port.c:151`）用 `bflb_flash_read()` 把 ROM 分区整段读进 PSRAM 的 `g_nes_rom[]`（`infones_port.c:50`，2 MB），`InfoNES_ReadRom`（`infones_port.c:237`）解析 iNES 头、把 `ROM`/`VROM` 指针指向它。**换游戏只烧 ROM 分区，无需重编固件**。

`load_rom_from_flash()`（`infones_port.c:58`）逐 256B 小块从 Flash 读（避免让 Flash 控制器直 DMA 到 PSRAM），先校验 `NES\x1a` 头，再按 `PRG×16KB + CHR×8KB + trainer` 算总长。无 ROM 时串口打印 `no valid ROM in flash @0x100000`，NES 空闲、UVC 走测试图案。

### 8.2 内存布局（全部大缓冲进 PSRAM）

| 缓冲 | 大小 | 位置 | 说明 |
| --- | --- | --- | --- |
| `g_nes_rom[]` | 2 MB | PSRAM | 从 Flash `0x100000` 读入的 `.nes` |
| `yuyv_buf[]` | 491 KB（512×480×2） | PSRAM（`hw_mjpeg.c:45`） | HW MJPEG 的 YUYV 输入 |
| `jpeg_buf[]` | 256 KB（`MJPEG_BUF_SZ`） | PSRAM（`infones_port.c:125`） | 单帧 MJPEG 输出 |
| `nes_audio_ring[]` | ~22 KB | PSRAM | APU→UAC 立体声环形 |
| InfoNES 核心（WorkFrame/ChrBuf/PPURAM/RAM/SRAM） | ~184 KB | PSRAM（链接 `ram_psram` 区） | 模拟器内部状态 |

> 链接阶段 `ram_psram` 区使用率约 **95%**（余量紧张）。新增 PSRAM 大缓冲前须确认不 overflow，或先释放其他缓冲（见 §10.4 的 LUT 风险）。

> **PSRAM 不清零**：`.psram_noinit` 段启动不保证清零，`nes_bridge_init()` 里显式 `memset` 所有模拟器缓冲（WorkFrame/ChrBuf/PPURAM/SRAM/RAM/wave_buffers/ApuEventQueue/nes_audio_ring/jpeg_buf）。

---

## 9. InfoNES 移植（可移植 API 与桥接）

InfoNES 核心通过一组平台函数与宿主交互，本项目在 `infones_port.c` 实现它们：

| 函数 | 作用 | 关键点 |
| --- | --- | --- |
| `InfoNES_ReadRom` | 解析 iNES 头、把 ROM/VROM 指针指向 `g_nes_rom` | 不改核心，直接喂 PSRAM 镜像 |
| `InfoNES_LoadFrame` | 每帧把 `WorkFrame` 编码成 MJPEG | `jpeg_state` 门控；FPS 计数（见 §10） |
| `InfoNES_SoundOutput` | 5 路波形 → 环形缓冲 | DC blocker + gain（§7.2） |
| `InfoNES_PadState` | 返回 PAD1/PAD2/System 位掩码 | 读 `pad_state`（§11） |
| `InfoNES_Wait` | 每帧让出 16ms | `PPU_Scanline==0` 时 `vTaskDelay(16)` → ~60fps 且让出 USB 任务 |
| `InfoNES_SoundInit` | 强制 44100/735 | 采样率对齐 |
| `InfoNES_MemoryCopy/Set` | 转调 `memcpy/memset` | — |
| `InfoNES_Menu/DebugPrint/MessageBox` | 桩 | 直接返回/忽略 |

**调色板**：`NesPalette[64]`（`infones_port.c:102`）用 FCEUX 标准 RGB565 调色板替换 InfoNES 默认（偏蓝暗）表；InfoNES 把它拷进 `PalTable[]` 再进 `WorkFrame`，改这个数组即改全局颜色。

**运行**：`nes_bridge_run()`（`infones_port.c:412`）→ `InfoNES_Load("")` → 成功置 `g_nes_running=1` → `InfoNES_Main()`（永不返回）。

---

## 10. 性能优化

### 10.1 瓶颈定位方法

`enc fps`（串口每秒打印）计的是 `InfoNES_LoadFrame()` 成功编码次数，受 `jpeg_state` 门控（USB 取走才复位）→ **`enc fps = min(编码速率, USB 消费速率)`**。

- `enc fps` ≪ 15（描述符声明值）→ 瓶颈在**编码侧**。
- `enc fps` ≥ 15 → USB 没卡描述符，瓶颈仍在**单帧 `T_enc`**（编码侧），只是 USB 消费更快。

实测 `enc fps=19~20` **超过** 15 → 瓶颈是单帧 `T_enc≈50ms`（NES 模拟 + RGB→YUYV + HW 编码 + dcache 维护），不在 USB/带宽。

### 10.2 已完成：软件 MJPEG → 硬件 MJPEG

初版纯软件定点 `fdct` 编码器（`soft_mjpeg.c`），256×240 偏慢、512×480 不可行。改用 `hw_mjpeg.c` 封装 BL616 硬件 MJPEG：DCT/量化/哈夫曼全硬件。结果 512×480@q90 下 `enc fps≈10`（编码本身不再是瓶颈，瓶颈转模拟+转换）。

### 10.3 已完成：② 去冗余 2× 转换 + ① 热点 -O2

| 优化 | 内容 | 效果 |
| --- | --- | --- |
| ② 去冗余 2× 转换 | `hw_mjpeg_encode` 的 2× 路径按 NES 像素遍历，每像素算一次 (Y,Cb,Cr) 填 2×2 块 | `rgb565_ycbcr` 调用 245760 → 61440（÷4） |
| ① 热点 -O2 | 对 K6502/InfoNES/InfoNES_Mapper/InfoNES_pAPU/infones_port/hw_mjpeg/frame_audio_source 设 `COMPILE_FLAGS "-O2"`，覆盖默认 `-Os` | 6502 解释器 + PPU + 逐像素运算提速 |

**结果**：`enc fps` 从 **10 → 19~20**（512×480 @q90）。编译通过（`Built target combine`，`ram_psram` ~95% 无 overflow），已烧录 COM6（SHA 校验通过）。

### 10.4 待实施：③ 候选（已分析，未动手）

| # | 手段 | 位置 | 预期 | 风险 |
| --- | --- | --- | --- | --- |
| A | RGB565→YUV 查表 LUT | 逐像素转换 | 61440 次/帧乘加 → 1 次查表（预建 65536 项 ≈192KB PSRAM） | 多占 192KB，余量紧张 |
| B | 描述符 `VIDEO_FPS` 15→30 | USB 协商 | 让主机能协商更高帧率（实测已超 15，提 30 可能释放更高 fps） | 极低 |
| C | yuyv 缓冲改 non-cacheable | dcache 维护 | 免每帧 491KB `dcache_clean_range` | CPU 写非缓存 PSRAM 更慢，需实测 |
| D | 热点 `-O3` | NES 模拟 | 6502 解释器可能再快 | 体积略增 |
| E | encode 计时打点 | 诊断 | 串口打印每帧编码 ms，先定位 encode vs emulate 各占多少 | 零风险 |

> 注：`InfoNES_LoadFrame` 的 FPS 计数行打印 `NES_DISP_WIDTH/HEIGHT`（恒 256×240，NES 源分辨率），**不是**实际流出的 512×480（§9.2 已知小瑕疵），纯日志标签问题，随 ③ 一并修。

---

## 11. HID OUT 手柄输入子系统

### 11.1 为什么走 HID OUT

BL616 是插在 PC 上的 USB **Device**，同一路 USB **不能同时当 Host 接手柄**。唯一零硬件路径是"PC 采集真手柄/键盘按键 → 经已有 USB 线写回 NES 按钮"。数据方向是 PC→板子，故用 **OUT 端点**（不是 IN）。

### 11.2 HID 报告描述符（逐字节，`usb_composite.c:82`）

```c
static const uint8_t hid_report_desc[] = {
    0x06, 0x00, 0xFF,   // Usage Page (Vendor 0xFF00)
    0x09, 0x01,         // Usage (1)
    0xA1, 0x01,         // Collection (Application)
    0x85, 0x01,         //   Report ID (1)
    0x09, 0x01,         //   Usage (1)
    0x15, 0x00,         //   Logical Minimum (0)
    0x25, 0xFF,         //   Logical Maximum (255)
    0x75, 0x08,         //   Report Size (8)
    0x95, 0x01,         //   Report Count (1)
    0x91, 0x02,         //   Output (Data,Var,Abs)   ← 主机→设备
    0xC0                // End Collection
};
```

**只声明一个 Output 报告**（`0x91` = Output），主机发 `bytes([1, nes_pad_byte])`（report id + 1 字节）。**没有 Input/Feature 报告**。

### 11.3 HID 类描述符 = 恰好 9 字节（致命坑复盘）

HID 类描述符（在 config 里，紧跟接口描述符）：

```c
USB_INTERFACE_DESCRIPTOR_INIT(0x04, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00),  // iface 4, HID 类
0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, WBVAL(HID_REPORT_DESC_LEN),     // HID 类描述符（9 字节）
USB_ENDPOINT_DESCRIPTOR_INIT(HID_OUT_EP, 0x03, 64, 0x01),                 // OUT EP 0x03, 64B
```

`0x09,0x21,...` 是 **HID 类描述符**：`bLength=0x09`、`bDescriptorType=0x21`、`bcdHID=0x0111`、`bCountry=0`、`bNumDescriptors=1`、`bDescriptorType=0x22(HID report)`、`wDescriptorLength=22`。因为只 1 个 Report 子描述符，**总长恰好 9 字节，没有任何尾字节**。

> **曾经致命的 bug**：手写时多写了 1 字节尾随 `0x00`，Windows 解析器把它当下一子描述符 `bLength=0` → 整份配置解析失败 → UVC/UAC/HID 全不出现。修复：删尾 `0x00`。编译后从 ELF `config_descriptor` 字节级走查确认 `wTotalLength=292`、5 接口齐全、HID 9 字节、OUT EP 0x03 紧随、整体 WELL-FORMED（建议沉淀 `tools/check_config_desc.py` 防回归）。

### 11.4 双通道收数据（任一均可）

1. **OUT 端点 0x03**（`hid_out_ep_cb`，`usb_composite.c:245`）：PC 写 `bytes([1, pad])` → 取 `hid_out_buf[1]` 调 `nes_bridge_set_pad()`，并 `usbd_ep_start_read` 重触发端点（中断端点必须武装才能再收）。
2. **Set_Report 控制通道（EP0）**（`usbd_hid_set_report`，`usb_composite.c:263`）：override 弱函数，取 `report[0]` 喂 `nes_bridge_set_pad()`，作无 OUT 端点发送器的兜底。

> **关键**：CherryUSB 的 `usbd_hid` 类驱动只管 EP0 的 Set/Get_Report；**中断端点数据必须自己注册 `ep_cb` 并重触发**，否则端点关闭、收不到。

### 11.5 收敛到 pad_state

`nes_bridge_set_pad()`（`infones_port.c:186`）只做 `pad_state = pad;`。`InfoNES_PadState()`（`infones_port.c:345`）每帧读 `pad_state` 填 PAD1。**任何数据源**（GPIO/BLE/串口/USB HID）算出 NES 位掩码调此函数即可。

**位掩码**（InfoNES PAD1，硬件侧 `infones_port.c:144`）：

| 位 | b7 | b6 | b5 | b4 | b3 | b2 | b1 | b0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 键 | Right | Left | Down | Up | Start | Select | B | A |

单字节值：`A=0x01 B=0x02 Select=0x04 Start=0x08 Up=0x10 Down=0x20 Left=0x40 Right=0x80`。

### 11.6 PC 发送工具

- `tools/nes_pad_sender.py`（推荐）：`pip install hid pynput`（键盘全局钩子）；`python -u tools/nes_pad_sender.py --keyboard` 用 `w/a/s/d`=↑←↓→、`j/k`=A/B、`Enter`=Select、`Space`=Start。手柄模式（不带 `--keyboard`）用 pygame 读真手柄。
  - **cython-hidapi 1.0.9 新 API**：旧 `hid.device().open()` 已移除，改 `hid.Device(vid,pid)`（构造即打开）、`dev.nonblocking=True`、`dev.product` 属性。
  - **Windows 坑**：`pip install hid` 的轮子不含原生 `hidapi.dll`；需从 hidapi 官方 release 取 x64 `hidapi.dll` 放 `venv/Scripts/` 并 `Unblock-File` 解除 Internet 封锁；脚本顶部 `os.add_dll_directory(exe目录)` 引导加载。
  - 后台运行必须 `python -u`（否则 print 被块缓冲吞掉）。
- `tools/nes_pad_webhid.html`（Chrome 零安装）：`file://` 不可用 WebHID，需 `python -m http.server` 起本地服务后开 `http://localhost:8000/...`；**非激活标签页收不到键**（浏览器安全限制），真后台读键请用 Python 脚本。

---

## 12. 烧录 / 测试 / 排错速查

### 12.1 烧录

工具 `BLFlashCommand.exe`（SDK 自带）。**板子无自动 DTR 复位，必须手动**：按住 BOOT → 轻点 RESET（BOOT 为低时进 bootrom）→ 保持 BOOT 跑命令 → 看到 `Handshake succeeded` 松 BOOT → `Verification succeeded` 完成。烧前关掉占用 COM6 的串口监视器（否则 `PermissionError`）。

```bash
D:/bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube/BLFlashCommand.exe \
  --port COM6 --config flash_prog_cfg_com6.ini --chipname bl616 write_flash_files
```

**固件只烧一次**；之后换游戏只烧 ROM：

```bash
python tools/flash_rom.py add C:/path/to/game.nes --port COM6   # 写目录区+数据，不动 0x0 固件
```

烧完**松开 BOOT、按 RESET（或重插 USB）**进正常模式。串口应见 `[NES] Main start`。

> **一次性分发**：把固件 + 全部已 `add` 的游戏拼成单个镜像一条命令烧完，见主 README §6.5（`tools/make_merged_firmware.py` + `flash_prog_cfg_merged.ini`）。

### 12.2 测试

| 项目 | 方法 | 预期 |
| --- | --- | --- |
| 枚举 | 设备管理器 | 相机(UVC) + 麦克风(UAC) + HID 厂商设备(接口4) 三设备齐全、无感叹号 |
| 画面 | Windows 相机 / OBS | 512×480、颜色正常、高对比边缘无红/绿鬼影 |
| 声音 | 语音录音机 | 录到 NES 音；空闲时为 1kHz 测试音 |
| 手柄 | 运行发送工具 | Contra 标题屏可用方向/A/B/Select/Start 操作 |

> Windows 探测 MS OS 描述符（`string 0xEE`）会打印 `not found` —— **无害**，不影响枚举。

### 12.3 排错

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 上电即 `exception_entry mcause=0x38000003` | `bflb_device_get_by_name("usb")` 找不到 | 必须用 `BFLB_NAME_USB_V2`（即 `"usb_v2"`） |
| `IMG LOAD HANDSHAKE FAIL` | 没进下载模式 | 按 §12.1 手动 BOOT+RESET |
| 颜色不对/偏移不同 | MJPEG range 不匹配 | 已改 studio range；仍偏暗查主机解码 |
| 红/绿鬼影 | 关了 2× | 确认 `NES_FRAME_UPSCALE_2X=1` |
| 音频刺耳 | GAIN 过高+无去直流 | 已改 DC blocker + `NES_AUDIO_GAIN=40` |
| 烧录 COM 被占 | 串口监视器占用 | 关闭再烧 |
| UVC/UAC 全消失 | HID 类描述符多 1 字节尾 | 见 §11.3（已修） |

---

## 13. 附录

### A. 关键常量速查

| 常量 | 值 | 位置 |
| --- | --- | --- |
| `NES_FRAME_UPSCALE_2X` | 1 | `uvc_uac.h:59` |
| `VIDEO_WIDTH/HEIGHT` | 512/480 | `uvc_uac.h:63` |
| `VIDEO_FPS` | 15 | `uvc_uac.h:69` |
| `VIDEO_IN_EP / AUDIO_MIC_EP / HID_OUT_EP` | 0x81/0x82/0x03 | `uvc_uac.h:47` / `usb_composite.c:80` |
| `USBD_VID/PID` | 0xABCD/0x1234 | `usb_composite.c:75` |
| `SOFT_MJPEG_QUALITY` | 90 | `infones_port.c:129` |
| `NES_AUDIO_GAIN / DC_SHIFT` | 40 / 8 | `infones_port.c:321` |
| `NES_ROM_FLASH_OFFSET / MAX_SIZE` | 0x100000 / 2MB | `infones_port.c:48` |
| `MJPEG_BUF_SZ` | 256KB | `infones_port.c:121` |

### B. 最小复现清单

1. `BL_SDK_BASE=D:/bouffalo_sdk CHIP=bl616 BOARD=bl616dk make`
2. 板子进 ISP，烧 `cherryusb_bl616.bin` 到 0x0
3. 烧一个 NROM/mapper2 的 `.nes` 到 0x100000（如 Contra）
4. 松 BOOT+RESET → 设备管理器出现相机/麦克风/HID
5. `python -u tools/nes_pad_sender.py --keyboard` → 敲 `w/a/s/d/j/k/Enter/Space` 玩

### C. 工程约定

- 只改工程 app 代码（`main.c` / `usb_composite.c` / `frame_audio_source.c` / `nes/`），**不碰 SDK**（CherryUSB 类驱动、链接脚本保持原样）。
- 大缓冲进 PSRAM（`.psram_noinit` + `NES_PSRAM_BSS`），并在 `nes_bridge_init()` 显式 `memset` 清零。
- 性能优化走增量：一次一个瓶颈（②+① → ③），每步编译+烧录+串口 `enc fps=` 对比，先用数据定位再动手。

---

## 14. 上电游戏选择菜单（Launcher）

固件上电**不再自动跑固定 ROM**，而是先在 UVC 视频流上画出游戏列表，用 HID
（方向键 + A/Start）选游戏，选中后从对应 Flash 槽加载并运行。游戏退出/出错
会回到菜单。

### 14.1 工作原理

- `nes_bridge_run()`（`nes/infones_port.c`）上电进入菜单循环：扫描 ROM 分区的
  **目录区**（方案 C，类文件系统最小实现），把每个有效目录项（其数据偏移处
  `NES\x1a` 头校验通过）列成菜单，用 `nes_menu_render()` 画到 256×240 RGB565
  缓冲，再由 `nes_bridge_push_menu_frame()` 走和 NES 帧**同一条**
  `hw_mjpeg_encode → jpeg_buf/jpeg_state → UVC` 通道播出。
- **ROM 分区布局**（`NES_ROM_FLASH_OFFSET = 0x100000`）：
  - `[0x100000]` 目录区 4KB：每条目 44B = 游戏名(32B UTF-8) + 偏移(4B) +
    长度(4B) + 标志(1B) + pad(3B)，最多 `ROM_DIR_MAX_ENTRIES=64` 条。
  - `[0x101000]` 数据区：各 ROM 顺次存放、4KB 对齐。名字来自烧录时
    `flash_rom.py` 用的文件名，随 ROM 一起存进目录区 —— **即文件名=游戏名**，
    没有硬编码槽表，烧错位置也不会错配名字。
- **菜单帧缓冲复用 `g_nes_rom` 前 120KB**（菜单态 ROM 区空闲），零额外内存。
- 中文显示靠构建期生成的点阵字体：`tools/gen_menu_font.py` 用主机
  `PIL + simhei.ttf` 把菜单用到的字渲染成 16×16 单色点阵，写入
  `nes/nes_menu_font.h` 编进固件（约 1.8KB，含 57 字形）。
- HID 导航复用现有位掩码（见 §11）：`Up=b4 / Down=b5 / A=b0 / Start=b3`。
  `nes_bridge_run` 做**边沿触发滚动 + 按住 400ms 自动重复**，选中（A 或 Start
  边沿）调用 `launch_and_run(目录项偏移)` 加载运行。PC 端 `nes_pad_sender.py` /
  `nes_pad_webhid.html` **无需改动**。

### 14.2 烧录 / 管理游戏（类文件系统）

`tools/flash_rom.py` 把 `.nes` 当作"文件"管理，PC 端用 `rom_manifest.json`
记录已烧录项（已加入 `.gitignore`）。游戏名默认取文件名（去 `.nes`），
可用 `--name` 覆盖。固件只烧一次（含菜单）；之后增删游戏**不重编固件**：

```bash
# 追加（名字默认取文件名；可用 --name 指定）
python tools/flash_rom.py add 超级玛丽.nes --port COM6
python tools/flash_rom.py add 双截龙.nes   --port COM6
python tools/flash_rom.py add 魂斗罗.nes   --port COM6
# 列出当前已烧录
python tools/flash_rom.py list
# 删除（只改目录区，数据留作碎片）
python tools/flash_rom.py remove 双截龙 --port COM6
# 清空目录区（菜单显示"未找到游戏"）
python tools/flash_rom.py reset --port COM6
```

每次 `add`/`remove` 都会重写目录区（4KB，写 `0x100000`，`erase=1`），
新 ROM 数据写到计算出的 4KB 对齐偏移（`erase=1`）。烧前板子须进下载模式（§12.1）。
烧完正常模式上电，UVC 画面即出现中文游戏列表（方向键移动、A/Start 开始）。

### 14.3 增删游戏 / 改显示名

- **加游戏**：`add` 即可，名字自动取自文件名；新字若未在点阵字体里，菜单会显示
  缺字方框 —— 把字补进 `tools/gen_menu_font.py` 的 `STRINGS`，重跑
  `python tools/gen_menu_font.py` 重新生成字体，再 `make` 烧固件。
- **改显示名**：`add` 时加 `--name 新名`（同名会覆盖旧项）。
- 目录项上限 `ROM_DIR_MAX_ENTRIES=64`；数据区约 7MB（8MB Flash 减去固件与
  4KB 目录），单 ROM 上限 `NES_ROM_MAX_SIZE=2MB`。

