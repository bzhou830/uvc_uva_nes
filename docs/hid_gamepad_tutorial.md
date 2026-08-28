# BL616 UVC+UAC+NES 手柄输入（USB HID OUT）详尽技术教程

> 适用范围：本教程聚焦 **手柄输入子系统**（最近新增的 `iface 4` HID，通过 USB 中断 OUT 端点把 PC 按键转发到 BL616 上的 NES 模拟器）。系统其余部分（UVC 摄像头、UAC 麦克风、硬件 MJPEG、InfoNES 模拟）仅作为上下文背景。所有行号、宏名、字节均取自 `uvc_uac_nes/` 当前实现。

---

## 0. 架构一句话

```
PC 键盘/手柄  ──(USB 中断 OUT 0x03, [report_id=1][nes_pad_byte])──▶  BL616 (USB Device)
                                                                          │
                                                                          ▼ nes_bridge_set_pad()
                                                                   pad_state (volatile uint32)
                                                                          │
                                                                          ▼ InfoNES_PadState() 每帧读取
                                                                      NES PAD1 按钮位
```

---

## 1. 为什么走 USB HID OUT（系统设计约束）

这是整个方案成立的前提，必须先讲清。

- **BL616 是 USB Device（插在 PC 上）**，同一路 USB 硬件在某一时刻要么当 Device、要么当 Host，不能既作为相机被 PC 枚举、又作为 Host 去接有线手柄。因此“板子直接插手柄”这条路被硬件角色封死。
- 无线手柄（蓝牙）同样需要板子当 Host/双模栈，BL616 当前工程没有实现，且会增加射频与协议栈复杂度。
- **唯一零额外硬件的路径**：PC 上本来就有手柄/键盘（或用户手边的游戏手柄），由 PC 采集按键，再经 **已经存在的那根 USB 线** 写回板子。板子作为 USB Device 接收主机发来的数据——这在 USB 语义里就是 **OUT 端点**。

> 关键洞察（也是最常见的理解误区）：手柄是“输入设备”，但数据流动方向是 **PC → 板子**（主机 → 设备），所以在设备侧它必须用一个 **OUT 端点** 来收。这和“鼠标用 IN 端点上报移动”正好相反。

- **为什么选 HID 类而不是自定义 vendor 类**：HID 是 Windows 原生支持的类别（`hidusb.sys` 驱动免装），PC 端既能用 `hidapi`/`cython-hidapi`/`pywin32` 这类原生库，也能用浏览器 **WebHID**，无需写驱动、无需签名。

---

## 2. USB 协议原理（与本文相关的最小集）

### 2.1 端点方向：IN / OUT

| 方向位 | 含义 | 本设备例子 |
|---|---|---|
| IN (`0x80`) | 设备 → 主机 | `0x81` UVC 视频流、`0x82` UAC 麦克风 |
| OUT (`0x00`) | 主机 → 设备 | `0x03` 手柄输入（本教程主角） |

端点地址 = 方向位(高 1 位) | 端点号(低 4 位)。所以 `0x03` 的二进制是 `0000_0011`，方向位为 0 ⇒ OUT；端点号 3。`0x81` 是 `1000_0001` ⇒ IN、端点 1。

### 2.2 复合设备与接口（Interface）

单个 USB 设备可以包含多个 **接口（Interface）**，每个接口承载一类功能，由主机分别枚举为独立功能单元。本设备用 `bDeviceClass = 0xEF`（Miscellaneous Device）+ **IAD（Interface Association Descriptor）** 声明自己是一个复合设备，于是 Windows 会把它拆成多个子设备：

```
iface 0  VideoControl  (UVC VC)
iface 1  VideoStream   (UVC VS, 等时 IN 0x81)
iface 2  AudioControl  (UAC AC)
iface 3  AudioStream   (UAC AS mic, 等时 IN 0x82)
iface 4  HID           (本教程的 NES pad 接收器, 中断 OUT 0x03)   ← 新增
```

设备管理器里会看到：相机（UVC）、麦克风（UAC）、HID 厂商设备（与 `BL6x8_UVC_UAC_DEMO` 同 PID，接口 4）。

### 2.3 HID 类与 Report Descriptor

HID 类不使用固定二进制结构，而是用一份 **Report Descriptor（报告描述符）** 自描述数据的含义。报告分三类：

- **Input**：设备 → 主机（如鼠标移动、键盘按键上报）。
- **Output**：主机 → 设备（如键盘的 NumLock LED 状态）。**本设计用它来承载 NES 按钮位**——主机把一字节 NES 按键掩码写成 Output 报告发给板子。
- **Feature**：双向控制项（本设计未用）。

主机发 Output 报告有两条物理路径：
1. **中断 OUT 端点**（主路径）：通过 `0x03` 中断端点批量写，低延迟、无需握手。
2. **Set_Report 控制传输**（兜底路径）：通过端点 0（EP0）的 `SET_REPORT` 类请求发送，适合不支持中断 OUT 的简陋主机栈。

本固件 **两条都实现**：中断 OUT 是热路径，`Set_Report` 作 fallback。

---

## 3. CherryUSB 设备栈原理

本工程用 Bouffalo SDK 内置的 **CherryUSB** 作 Device 端协议栈。理解它对 HID 的实现方式，是写对手柄接口的关键。

### 3.1 描述符注册 vs 接口/端点注册（三者分离）

```c
usbd_desc_register(busid, &composite_descriptor);          // 整份设备/配置/字符串描述符（你手写）
usbd_add_interface(busid, usbd_hid_init_intf(...));        // 注册 HID 类接口（处理 EP0 类请求）
usbd_add_endpoint (busid, &hid_out_ep);                    // 注册中断 OUT 端点（搬运数据）
```

- `usbd_desc_register`：把你手写的 `config_descriptor[]` 原样交给栈，枚举时回给主机。
- `usbd_add_interface(usbd_hid_init_intf(...))`：把 HID **类驱动**挂到接口 4。类驱动只负责 **EP0 上的类请求**（`GET_REPORT` / `SET_REPORT`），以及把报告描述符回给主机的 `GET_DESCRIPTOR(HID)` 请求。
- `usbd_add_endpoint`：真正让 `0x03` 这个中断端点工作，数据到达时触发你提供的 `ep_cb`。

### 3.2 最关键的一点：HID 类驱动**不管**中断端点的数据

很多人第一次写会以为“加了 `usbd_hid_init_intf` 手柄数据就自动进来了”，结果收不到。`usbd_hid.c` 只管控制通道（EP0）的 Set/Get_Report，**中断 IN/OUT 端点的数据搬运完全不在类驱动里**。你必须：

1. 用 `usbd_add_endpoint` 注册 `0x03` 并绑定回调 `hid_out_ep_cb`；
2. 主动调用 `usbd_ep_start_read(busid, HID_OUT_EP, buf, len)` 武装端点（告诉硬件“下一包数据 DMA 到 buf”）；
3. 在回调里读完数据后 **再次 `usbd_ep_start_read`** 重触发，否则端点会关闭、之后不再收数据（相当于一次性缓冲）。

这与 CherryUSB 的 CDC/ADC 范例（如 `usbd_adc.c` 的 OUT 端点处理）是同一套路。

---

## 4. 固件实现（逐文件）

### 4.1 `defconfig` —— 编译开关

```ini
CONFIG_CHERRYUSB_DEVICE_HID =y
```

这一行决定 `libcherryusb` 是否编入 `usbd_hid.c`。**缺它，`usbd_hid_init_intf()` 和 `usbd_hid_set_report` 链接不到**，编译/链接失败，或运行时接口不工作。其余 UVC/UAC 开关（`CONFIG_CHERRYUSB_DEVICE_VIDEO` / `_AUDIO`）此前已有。

### 4.2 `usb_composite.c` —— HID 描述符

#### 4.2.1 端点常量与报告描述符

```c
#define HID_OUT_EP  0x03        /* 中断 OUT，方向位为 0；VC=0x81 / AC=0x82 已占用，0x03 空闲 */

/* 1 字节 Output 报告，Report ID = 1，vendor usage page 0xFF00 */
static const uint8_t hid_report_desc[] = {
    0x06, 0x00, 0xFF,   /* Usage Page (Vendor 0xFF00) */
    0x09, 0x01,         /* Usage (1) */
    0xA1, 0x01,         /* Collection (Application) */
    0x85, 0x01,         /*   Report ID (1) */
    0x09, 0x01,         /*   Usage (1) */
    0x15, 0x00,         /*   Logical Minimum (0) */
    0x25, 0xFF,         /*   Logical Maximum (255) */
    0x75, 0x08,         /*   Report Size (8) */
    0x95, 0x01,         /*   Report Count (1) */
    0x91, 0x02,         /*   Output (Data,Var,Abs) */
    0xC0                /* End Collection */
};
#define HID_REPORT_DESC_LEN  (sizeof(hid_report_desc))   /* = 22 字节 */
```

报告描述符逐字节语义：

| 字节 | 值 | HID 项 | 含义 |
|---|---|---|---|
| 0–2 | `06 00 FF` | Usage Page | 厂商自定义页 `0xFF00`（不与标准 HID 页冲突） |
| 3–4 | `09 01` | Usage | 用法 1 |
| 5–6 | `A1 01` | Collection | Application 集合开始 |
| 7–8 | `85 01` | Report ID | 报告 ID = 1（PC 发送时首字节须为 1） |
| 9–10 | `09 01` | Usage | 用法 1 |
| 11–12 | `15 00` | Logical Min | 0 |
| 13–14 | `25 FF` | Logical Max | 255（一字节全范围） |
| 15–16 | `75 08` | Report Size | 每字段 8 bit |
| 17–18 | `95 01` | Report Count | 1 个字段 ⇒ 一字节报告 |
| 19–20 | `91 02` | Output | **Output**（主机→设备），Data/Var/Abs |
| 21 | `C0` | End Collection | 集合结束 |

总计 22 字节，`HID_REPORT_DESC_LEN = 22`。

#### 4.2.2 描述符尺寸计算

```c
#define HID_DESC_PART  (9 + 9 + 7)   /* 接口描述符(9) + HID 类描述符(9) + OUT 端点(7) */
#define USB_COMPOSITE_DESC_SIZ  (unsigned long)(9 + UVC_DESC_PART + UAC_DESC_PART + HID_DESC_PART)
```

`USB_COMPOSITE_DESC_SIZ` 必须严格等于整份配置描述符的字节数（含 9 字节配置头），否则主机解析长度对不上会拒绝设备。**改了任何描述符都要同步改这里的尺寸**。

#### 4.2.3 配置描述符里的 HID 接口块

```c
/* config_descriptor[] 中，接在 UAC AS 之后： */
USB_INTERFACE_DESCRIPTOR_INIT(0x04, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00),
/*   ↑ iface=4  alt=0  1个端点  类=0x03(HID)  子类=0x00  协议=0x00  无接口字符串 */

0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, WBVAL(HID_REPORT_DESC_LEN),
/*  ↑ HID 类描述符（9 字节，正好，无尾随字节！） */

USB_ENDPOINT_DESCRIPTOR_INIT(HID_OUT_EP, 0x03, 64, 0x01),
/*  ↑ 端点 0x03  类型=0x03(中断)  MPS=64  轮询间隔=1 */
```

**HID 类描述符（0x21）逐字节**：

| 偏移 | 值 | 字段 |
|---|---|---|
| 0 | `09` | bLength = 9 |
| 1 | `21` | bDescriptorType = 0x21 (HID) |
| 2–3 | `11 01` | bcdHID = 0x0111（HID 1.11） |
| 4 | `00` | bCountryCode = 0（不限国家） |
| 5 | `01` | bNumDescriptors = 1（下面跟 1 个子类描述符） |
| 6 | `22` | bDescriptorType = 0x22（Report） |
| 7–8 | `WBVAL(22)` | wDescriptorLength = 22（=HID_REPORT_DESC_LEN） |

> ⚠️ **这里就是之前导致 UVC/UAC 整设备消失的致命坑**：HID 类描述符含 1 个 Report 子描述符时**正好 9 字节**。若手滑在多写 1 个尾随 `0x00`，主机会按 `bLength=9` 读完 HID 类描述符，下一个字节正好是那个 `0x00`，被误当成“下一个子描述符的 bLength” ⇒ `bLength=0` 非法 ⇒ **整份 config 解析失败 ⇒ UVC/UAC/HID 全不出现**。修复就是删掉尾随 `0x00`，并保留恰好 9 字节。编译不会报错，但设备不可枚举——务必从 ELF 字节走查确认（见 §8）。

### 4.3 `usb_composite.c` —— 运行期缓冲与回调

#### 4.3.1 缓冲（DMA 安全）

```c
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_out_buf[64];
```

放在 `USB_NOCACHE_RAM_SECTION` 且对齐，确保 USB DMA 写入时不被 D-Cache 别名干扰（与 `video_packet_buf`/`audio_write_buf` 同样的处理）。

#### 4.3.2 配置完成后武装 OUT 端点

```c
static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    switch (event) {
    case USBD_EVENT_RESET:
        video_streaming = false; audio_streaming = false;
        video_busy = false;      audio_busy = false;
        break;
    case USBD_EVENT_CONFIGURED:
        /* 主机 Set_Configuration 后，第一次武装 HID OUT 端点 */
        usbd_ep_start_read(busid, HID_OUT_EP, hid_out_buf, sizeof(hid_out_buf));
        break;
    }
}
```

只在 `USBD_EVENT_CONFIGURED`（主机选中配置）时武装一次，之后每次回调自行重触发。

#### 4.3.3 中断 OUT 端点回调（主路径）

```c
static void hid_out_ep_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep;
    /* 包布局：[report_id=1][nes_pad_byte]；只在收到完整报告时处理 */
    if (nbytes >= 2) {
        nes_bridge_set_pad(hid_out_buf[1]);   /* 取 report_id 之后的字节 */
    }
    /* 重触发 OUT 端点，准备下一包 */
    usbd_ep_start_read(busid, HID_OUT_EP, hid_out_buf, sizeof(hid_out_buf));
}

static struct usbd_endpoint hid_out_ep = {
    .ep_cb   = hid_out_ep_cb,
    .ep_addr = HID_OUT_EP,
};
static struct usbd_interface hid_intf;   /* HID (iface 4) */
```

PC 每发一包 `[1, pad]`，DMA 落入 `hid_out_buf`，回调取其 `[1]`（跳过 report_id）喂给 `nes_bridge_set_pad`，再 `usbd_ep_start_read` 武装下一包。

#### 4.3.4 Set_Report 控制路径兜底（fallback）

```c
void usbd_hid_set_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t *report, uint32_t report_len)
{
    (void)busid; (void)intf; (void)report_id; (void)report_type;
    if (report_len >= 1) {
        nes_bridge_set_pad(report[0]);   /* Set_Report 路径下 report[0] 直接是 NES 字节 */
    }
}
```

CherryUSB 把 `usbd_hid_set_report` 声明为 `__WEAK`，这里在自己的文件里**重新定义**即覆盖默认空实现，接收 EP0 Set_Report 控制传输里的 NES 字节。注意：Set_Report 路径没有 report_id 前缀，`report[0]` 本身就是按钮位；中断 OUT 路径则 `hid_out_buf[1]` 才是按钮位（因为 `hid_out_buf[0]=report_id`）。两条路径最终都收敛到 `nes_bridge_set_pad`。

#### 4.3.5 接口/端点注册

```c
void uvc_uac_init(uint8_t busid, uintptr_t reg_base)
{
    usbd_desc_register(busid, &composite_descriptor);
    /* 顺序须与描述符一致：VC(0) VS(1) AC(2) AS(3) */
    usbd_add_interface(busid, usbd_video_init_intf(...));
    usbd_add_interface(busid, usbd_video_init_intf(...));
    usbd_add_interface(busid, usbd_audio_init_intf(...));
    usbd_add_interface(busid, usbd_audio_init_intf(...));
    usbd_add_endpoint(busid, &video_in_ep);   /* 0x81 */
    usbd_add_endpoint(busid, &audio_in_ep);   /* 0x82 */

    /* HID pad 接收器：注册接口（带报告描述符）+ OUT 端点 */
    usbd_add_interface(busid, usbd_hid_init_intf(busid, &hid_intf,
                                                 hid_report_desc, HID_REPORT_DESC_LEN));
    usbd_add_endpoint(busid, &hid_out_ep);    /* 0x03 */

    usbd_initialize(busid, reg_base, usbd_event_handler);
}
```

### 4.4 `nes/infones_port.c` —— NES 钩子（接收端）

```c
/* Joypad (bitmask matches InfoNES PAD1: A=b0 B=b1 Select=b2 Start=b3
 *                                    Up=b4 Down=b5 Left=b6 Right=b7) */
static volatile uint32_t pad_state = 0;

void nes_bridge_set_pad(uint32_t pad)
{
    pad_state = pad;            /* HID 回调写这里 */
}

void InfoNES_PadState(DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem)
{
    *pdwPad1 = pad_state;       /* InfoNES 内核每帧读这里作为 PAD1 */
}
```

`pad_state` 是 `volatile` 全局变量，HID 中断回调（可能在 USB 中断上下文）写它，InfoNES 模拟主循环每帧通过 `InfoNES_PadState` 读它。无锁、单写者单读者，且只写整字节，对 NES 来说足够（按键是电平语义，非边沿）。

**按钮位掩码（必须和 PC 端、InfoNES 完全一致）**：

| NES 键 | 位 | 值 | PC 键（本工程映射） |
|---|---|---|---|
| A | b0 | `0x01` | `j` |
| B | b1 | `0x02` | `k` |
| Select | b2 | `0x04` | `Enter` |
| Start | b3 | `0x08` | `Space` |
| Up | b4 | `0x10` | `w` |
| Down | b5 | `0x20` | `s` |
| Left | b6 | `0x40` | `a` |
| Right | b7 | `0x80` | `d` |

PC 端把对应键位置位后 OR 成一字节 `pad`，与上面逐位对应。

---

## 5. 数据通路端到端时序

```
[PC] pynput on_press('w')  → pressed={0x10} → send() → dev.write(bytes([1, 0x10]))
        │  USB 中断 OUT 0x03
        ▼
[BL616] USB DMA → hid_out_buf = [1, 0x10] → hid_out_ep_cb → nes_bridge_set_pad(0x10)
        │  pad_state = 0x10
        ▼
[BL616] InfoNES 主循环下一帧 → InfoNES_PadState() → *pdwPad1 = 0x10 → NES 读得上=按下
```

- **无流控、无确认**：PC 每检测到按键状态变化就发一包；板子每 NES 帧采样 `pad_state`，因此“按住中连续触发”由 PC 端 pynput 钩子通过“按下加入集合、松开移除集合、每变化发一次”自然保证，无需板子做边沿检测。
- **多键同按**：NES 按钮位是 OR 语义，`pad` 可同时含多个位（如 `0x11` = 上+A），pynput 的 `pressed` 集合天然支持。

---

## 6. PC 转发实现

### 6.1 `tools/nes_pad_sender.py`（推荐，后台可玩）

#### 6.1.1 Windows 上加载原生 `hidapi.dll`

`pip install hid` 在 Windows 装的是 **cython-hidapi**，它**不打包原生 `hidapi.dll`**，需要系统能找到该 DLL：

1. 从 hidapi 官方 release 下载 `hidapi-win.zip`（注意资源名是 `hidapi-win.zip`，不是 `hidapi-win-0.14.0.zip`），解压取 **x64** 的 `hidapi.dll`。
2. 放到 venv 的 `Scripts/` 目录（即 Python 解释器所在目录）。
3. **解除 Internet 标记**：从网上下载的 DLL 带 MOTW，Windows/Defender 会阻止 `LoadLibrary`，须 `Unblock-File` 解除，否则 `import hid` 失败。
4. **DLL 搜索路径**：Python 3.8+ 默认不把 exe 所在目录加入 `ctypes` 的 DLL 搜索路径，裸名 `ctypes.CDLL('hidapi.dll')` 找不到。脚本顶部已加：

```python
import os, sys
if sys.platform == "win32":
    try:
        os.add_dll_directory(os.path.dirname(sys.executable))
    except (OSError, ValueError):
        pass
```

#### 6.1.2 cython-hidapi 1.0.x 新 API（旧代码会 AttributeError）

旧教程里的 `hid.device().open()` 在 1.0.x 已被移除，本脚本已适配：

```python
import hid
try:
    # 构造即打开（旧 API 的 hid.device() + .open() 已删除）
    dev = hid.Device(vid=args.vid, pid=args.pid)
except Exception as e:
    print("无法打开 HID 设备 ...")
    return
dev.nonblocking = True                       # 旧 API 的 set_nonblocking(True)
print("HID 打开:", dev.product)              # 旧 API 的 get_product_string() → .product 属性
```

`write` / `read` / `close` 接口不变。发送格式：

```python
buf = bytes([REPORT_ID, pad])   # REPORT_ID=1，pad=8 位 NES 按钮掩码
dev.write(buf)
```

#### 6.1.3 键盘钩子（pynput，支持长按/多键同按）

```python
from pynput import keyboard as pnk
keymap = {
    'w': BIT_UP, 'a': BIT_LEFT, 's': BIT_DOWN, 'd': BIT_RIGHT,
    'j': BIT_A, 'k': BIT_B,
    'enter': BIT_SELECT, 'space': BIT_START,
}
pressed = set()
def send():
    pad = 0
    for b in pressed: pad |= b
    dev.write(bytes([REPORT_ID, pad]))
def on_press(key):
    bit = keymap.get(name_of(key))
    if bit is not None:
        pressed.add(bit); send()
def on_release(key):
    bit = keymap.get(name_of(key))
    if bit is not None:
        pressed.discard(bit); send()
listener = pnk.Listener(on_press=on_press, on_release=on_release)
listener.start()
```

`pynput` 是**操作系统级键盘钩子（WH_KEYBOARD_LL）**，不依赖窗口焦点——边看摄像头画面边敲键也能转发（这是它优于 WebHID 的地方）。未装 `pynput` 时回退 `msvcrt`（仅点按、长按不可靠）。

#### 6.1.4 真手柄模式（pygame，默认不带 `--keyboard`）

```python
import pygame
# pygame.joystick 轮询摇杆轴/按钮/hat → build_pad(...) → dev.write
```

`pygame` 体积大、慢网可后置安装，不影响键盘模式。

#### 6.1.5 运行

```bash
"/c/Users/lx/.workbuddy/binaries/python/envs/default/Scripts/python.exe" -u tools/nes_pad_sender.py --keyboard
```

`-u` 必须加：后台管道下 Python 的 stdout 是块缓冲（4KB），pynput 阻塞循环永不刷，不加就看不到“已连接”日志。

### 6.2 `tools/nes_pad_webhid.html`（Chrome 零安装）

- 经 `http://localhost` 或 `https` 打开（`file://` 不支持 WebHID）。
- 点「连接」选 `ABCD:1234`，点屏上按钮（含 D-pad 的 →=bit128）或敲 `w/a/s/d/j/k/Enter/Space`。
- 发送同样 `bytes([1, pad]`。
- 局限：**标签页必须聚焦**才收得到键盘事件（浏览器安全模型），不适合后台边看边玩；适合偶尔手动点按。

---

## 7. 构建与烧录

```bash
cd /d/bouffalo_sdk/examples/cherryusb/uvc_uac_nes
make                                   # CMake 已含 nes/ + usb_composite.c
```

`defconfig` 须含 `CONFIG_CHERRYUSB_DEVICE_HID =y`（`make` 用它生成 `.config`；若改过 defconfig 需 `make clean` 或重生成）。

烧录（应用区更新，`erase=1` 只擦已编程区，boot2/partition/mfg 持久）：

```bash
BLFlashCommand.exe --port COM6 --config flash_prog_cfg_com6.ini --chipname bl616 write_flash_files
```

烧完 **松开 BOOT → 按一次 RESET（或重插 USB）** 进正常模式。XIP 校验 host SHA == dev SHA 即烧录正确。

---

## 8. 验证与排错

### 8.1 设备枚举验证

设备管理器应同时出现：相机(UVC) + 麦克风(UAC) + HID 厂商设备（接口 4），三设备均无感叹号。

### 8.2 PC 端枚举

```bash
python tools/nes_pad_sender.py --list
```

应列出 `ABCD:1234  BL6x8_UVC_UAC_DEMO`（usage `FF00:0001`，MI_04）。

### 8.3 连接日志

```text
HID 打开: ABCD:1234  BL6x8_UVC_UAC_DEMO
键盘模式(pynput): w/a/s/d=↑←↓→  j/k=A/B  Enter=Select  Space=Start  (Ctrl+C 退出)
```

### 8.4 排错表

| 现象 | 根因 | 修复 |
|---|---|---|
| UVC/UAC 设备全消失 | HID 类描述符多出尾随 `0x00`（非 9 字节）→ 整份 config 解析失败 | 删尾字节，保持在 `config_descriptor` 里 HID 类描述符恰好 9 字节；编译后从 ELF 字节走查 `wTotalLength`/接口数确认 |
| `import hid` 失败 (Windows) | 缺原生 `hidapi.dll` 或 DLL 被 MOTW 封锁 | 放 x64 `hidapi.dll` 到 venv/Scripts + `Unblock-File` |
| `ctypes.CDLL('hidapi.dll')` 裸名找不到 | Python 3.8+ 不搜 exe 目录 | 脚本顶部 `os.add_dll_directory(os.path.dirname(sys.executable))` |
| `AttributeError: module 'hid' has no attribute 'device'` | cython-hidapi 1.0.x 移除旧 API | 改 `hid.Device(vid=,pid=)`；`.nonblocking=`；`.product` |
| 后台运行看不到连接日志 | stdout 块缓冲，pynput 阻塞不刷 | 加 `python -u` |
| 报“无法打开 HID 设备 ABCD:1234” | 板子还在 ISP 模式 / USB 没重插 / HID 设备未枚举 | 松 BOOT+RESET 进正常模式，设备管理器确认 HID 设备出现 |

### 8.5 描述符回归防护（强烈建议）

手写复合描述符后，务必从 ELF 字节走查（编译不会抓逻辑描述符 bug）：解析 `.symtab` 定位 `config_descriptor` 的 VMA，按 `wTotalLength` 切片，逐子描述符走查。期望结果：`wTotalLength=292`、`bNumInterfaces=5`、接口集 `{0,1,2,3,4}` 齐全、HID 类描述符 `bLength=9 / bNumDescriptors=1 / wReportLen=22`、OUT 端点 `0x03` 紧随其后、解析器恰好在 `wTotalLength` 处停住 ⇒ WELL-FORMED。可沉淀为 `tools/check_config_desc.py`。

---

## 9. 扩展与调优

- **真手柄**：装 `pygame`，默认模式即读摇杆/按钮，映射见 `gamepad_loop`（方向键/左摇杆=方向，按钮 0/1=A/B，6/7=Sel/Sta）。
- **双玩家**：可再加一个 Report ID（报告描述符加 `0x85,0x02` 集合）或第二个 HID 接口，分别喂 `PAD1`/`PAD2`。
- **提帧率**：当前 UVC 描述符 `VIDEO_FPS=15`，实际编码 `T_enc≈50ms`（19~20fps）已被描述符 15fps 卡住；若要 >15，需把 `VIDEO_INTERVAL_NS` 对应的帧间隔从 15 提到 30（HS 等时带宽足够），前提是 `T_enc<33ms`。
- **低延迟**：中断 OUT 端点轮询间隔已为 1（每帧），PC 端每变化即发，端到端延迟仅一个 USB 微帧（≈125µs@HS），对 NES 足够。

---

## 附：端到端最小复现清单

1. `defconfig` 加 `CONFIG_CHERRYUSB_DEVICE_HID =y`
2. `usb_composite.c`：加 HID 报告描述符、`config_descriptor` 的 iface4 块（9+9+7，HID 类描述符恰好 9 字节）、`hid_out_ep_cb`、`usbd_hid_set_report`、注册接口+端点、CONFIGURED 时武装端点
3. `infones_port.c`：`pad_state` + `nes_bridge_set_pad` + `InfoNES_PadState` 返回 `pad_state`
4. `make` 编译，`BLFlashCommand` 烧录，松 BOOT+RESET 进正常模式
5. PC：`python -u tools/nes_pad_sender.py --keyboard`，敲 `w/a/s/d/j/k/Enter/Space` 操作 NES
