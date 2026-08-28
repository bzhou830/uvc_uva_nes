/*
 * infones_port.c - InfoNES portable-API implementation for BL6x8 UVC+UAC
 *
 * PHASE 2: this file is linked with the InfoNES core (nes/infones_core sources).
 * It implements the platform functions the core calls, and the bridge
 * helpers consumed by frame_audio_source.c / main.c.
 *
 * Frame path:  InfoNES_LoadFrame() encodes the core's WorkFrame (256x240
 *              RGB565) to MJPEG into a SINGLE buffer guarded by jpeg_state,
 *              so the USB stack never reads a buffer mid-encode. (BL616 is
 *              single-core; a state flag is enough to avoid races.)
 * Audio path:  InfoNES_SoundOutput() mixes the 5 APU waves into a 16-bit
 *              stereo PCM ring; nes_bridge_get_audio() drains it for UAC.
 *
 * RAM budget note: InfoNES core already uses ~184 KB (WorkFrame 120,
 * ChrBuf 32, PPURAM 16, RAM/SRAM 16). We keep our extras small: one 56 KB
 * MJPEG buffer + a ~22 KB audio ring.
 */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "infones_core/InfoNES.h"
#include "infones_core/InfoNES_System.h"
#include "infones_core/InfoNES_pAPU.h"

#include "infones_port.h"
#include "uvc_uac.h"
#include "nes/hw_mjpeg.h"          /* BL616 hardware MJPEG encoder */

#include "log.h"
#define DBG_TAG "NESPRT"

/* --- profiling: RISC-V mcycle/mcycleh (CPU 320MHz) --- */
static inline uint64_t prof_now(void) {
    uint32_t lo, hi;
    asm volatile("csrr %0, mcycle\n csrr %1, mcycleh" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static uint64_t g_gap_us = 0;
static uint64_t g_last = 0;
static uint32_t g_gn = 0;

/* ------------------------------------------------------------------ *
 *  ROM image, loaded at boot from a DEDICATED FLASH PARTITION into this
 *  PSRAM buffer.  This decouples the game ROM from the firmware: to swap
 *  games you only re-flash the ROM partition (tools/flash_rom.py) -- no
 *  firmware rebuild.  Previously g_nes_rom[] was a const array compiled
 *  into the firmware.
 *
 *  Layout in g_nes_rom matches a raw .nes file:
 *    [0..15] = iNES header, [16..] = PRG, then CHR (after optional 512B
 *    trainer).  NES_ROM_FLASH_OFFSET is the physical flash address where
 *    the .nes is stored (see tools/flash_rom.py / flash_prog_cfg_rom.ini).
 * ------------------------------------------------------------------ */
#include "bflb_flash.h"
#define NES_ROM_FLASH_OFFSET  0x00100000UL   /* flash physical offset of ROM */
#define NES_ROM_MAX_SIZE      0x200000UL     /* 2 MB ROM buffer (in PSRAM)  */
static uint8_t  g_nes_rom[NES_ROM_MAX_SIZE] NES_PSRAM_BSS;
static uint32_t g_nes_rom_len = 0;

/* Read the ROM image from the dedicated flash partition into g_nes_rom.
 * Returns 0 on success, -1 if no valid NES image is present at the offset.
 * We chunk through a small SRAM temp buffer so we never ask the flash
 * controller to DMA straight into PSRAM; g_nes_rom is plain CPU-written
 * PSRAM, which is always safe. */
static int load_rom_from_flash(void)
{
    uint8_t hdr[16];
    g_nes_rom_len = 0;

    bflb_flash_read(NES_ROM_FLASH_OFFSET, hdr, sizeof(hdr));
    if (memcmp(hdr, "NES\x1a", 4) != 0) {
        LOG_I("[NES] no valid ROM in flash @0x%lx (expected 'NES\\x1a')\r\n",
              (unsigned long)NES_ROM_FLASH_OFFSET);
        LOG_I("[NES] flash a .nes to 0x%lx via tools/flash_rom.py\r\n",
              (unsigned long)NES_ROM_FLASH_OFFSET);
        return -1;
    }

    uint32_t prg     = hdr[4];
    uint32_t chr     = hdr[5];
    uint32_t trainer = (hdr[6] & 0x04) ? 512 : 0;
    uint32_t total   = 16 + trainer + prg * 16384UL + chr * 8192UL;
    if (total > NES_ROM_MAX_SIZE) {
        LOG_I("[NES] ROM %lu bytes exceeds buffer %lu, truncating\r\n",
              (unsigned long)total, (unsigned long)NES_ROM_MAX_SIZE);
        total = NES_ROM_MAX_SIZE;
    }

    uint8_t tmp[256];
    for (uint32_t off = 0; off < total; off += sizeof(tmp)) {
        uint32_t chunk = (total - off < sizeof(tmp)) ? (total - off) : sizeof(tmp);
        bflb_flash_read(NES_ROM_FLASH_OFFSET + off, tmp, chunk);
        memcpy(g_nes_rom + off, tmp, chunk);
    }
    g_nes_rom_len = total;
    LOG_I("[NES] ROM loaded from flash @0x%lx, len=%lu (PRG=%lu CHR=%lu trainer=%lu)\r\n",
          (unsigned long)NES_ROM_FLASH_OFFSET, (unsigned long)total,
          (unsigned long)prg, (unsigned long)chr, (unsigned long)trainer);
    return 0;
}

/* ------------------------------------------------------------------ *
 *  NES palette (RGB565, R-high: R bits 15..11, G 10..5, B 4..0).
 *  Required by InfoNES_System.h (extern); InfoNES copies these straight
 *  into PalTable[] and then into WorkFrame[], so this array IS the final
 *  on-screen color. Replaced the InfoNES default table (which is known to
 *  be dark / blue-shifted) with the widely-verified FCEUX default palette.
 * ------------------------------------------------------------------ */
WORD NesPalette[64] =
{
  0x52aa, 0x00ee, 0x0892, 0x3011, 0x400c, 0x5806, 0x5020, 0x38c0,
  0x2140, 0x09c0, 0x0200, 0x01e0, 0x0187, 0x0000, 0x0000, 0x0000,
  0x94b2, 0x0a78, 0x319d, 0x58fc, 0x88b5, 0x98ac, 0x9104, 0x79e0,
  0x52c0, 0x2b80, 0x0be0, 0x03a5, 0x032f, 0x0000, 0x0000, 0x0000,
  0xef7d, 0x4cdd, 0x7bfd, 0xab1c, 0xe2b4, 0xeacd, 0xe345, 0xc421,
  0x9520, 0x65e0, 0x2e20, 0x05ea, 0x0532, 0x0000, 0x0000, 0x0000,
  0xef7d, 0xa65d, 0xc5bd, 0xdd7c, 0xed18, 0xed11, 0xe56b, 0xd607,
  0xbe85, 0x96e3, 0x7f0a, 0x5eb3, 0x5e19, 0x0000, 0x0000, 0x0000
};

/* ------------------------------------------------------------------ *
 *  Single MJPEG buffer, guarded by jpeg_state (single-core BL616):
 *    0 = empty (encoder may write)
 *    1 = filled & free (USB may grab)
 *    2 = owned by USB (in flight; encoder must skip)
 * ------------------------------------------------------------------ */
#if NES_FRAME_UPSCALE_2X
#define MJPEG_BUF_SZ   (256 * 1024)  /* 512x480 frames need far more headroom */
#else
#define MJPEG_BUF_SZ   (80 * 1024)   /* headroom for higher quality frames */
#endif
static uint8_t  jpeg_buf[MJPEG_BUF_SZ] NES_PSRAM_BSS;
static uint32_t jpeg_len = 0;
static volatile uint8_t jpeg_state = 0;

#define SOFT_MJPEG_QUALITY  90   /* was 60: too lossy for text edges (high-freq
                                    quant attenuation). 90 keeps high-freq. */

/* ------------------------------------------------------------------ *
 *  Audio ring (SPSC: producer = NES task, consumer = main task).
 *  16-bit signed stereo; indices in int16 units.
 * ------------------------------------------------------------------ */
#define AUDIO_RING_SAMPLES  (AUDIO_SAMPLE_RATE / 8)   /* ~0.125 s */
static int16_t nes_audio_ring[AUDIO_RING_SAMPLES * AUDIO_CHANNELS] NES_PSRAM_BSS;
static volatile uint32_t audio_wr = 0;
static volatile uint32_t audio_rd = 0;
#define AUDIO_RING_CAP  (sizeof(nes_audio_ring) / sizeof(nes_audio_ring[0]))

/* Joypad (bitmask matches InfoNES PAD1: A=b0 B=b1 Select=b2 Start=b3
 * Up=b4 Down=b5 Left=b6 Right=b7). */
static volatile uint32_t pad_state = 0;

static int g_nes_running = 0;

/* ================================================================== *
 *  Bridge helpers
 * ================================================================== */
void nes_bridge_init(void)
{
    LOG_I("[NES] init: hw mjpeg encoder\r\n");
#if NES_FRAME_UPSCALE_2X
    hw_mjpeg_init(512, 480, SOFT_MJPEG_QUALITY);
#else
    hw_mjpeg_init(256, 240, SOFT_MJPEG_QUALITY);
#endif

    LOG_I("[NES] init: load ROM from flash\r\n");
    load_rom_from_flash();

    LOG_I("[NES] init: PSRAM memset start\r\n");
    /* Explicitly zero the PSRAM-resident emulator buffers. The .psram_bss
       section is NOT guaranteed to be cleared by the startup on BL616, and
       InfoNES relies on zero-initialised state. PSRAM is already initialised
       by board_psram_x8_init() (CONFIG_PSRAM) before main() runs. */
    memset(WorkFrame,  0, sizeof(WorkFrame));
    memset(ChrBuf,     0, 256 * 2 * 8 * 8);   /* ChrBuf[256*2*8*8] */
    memset(PPURAM,     0, PPURAM_SIZE);
    memset(SRAM,       0, SRAM_SIZE);
    memset(RAM,        0, RAM_SIZE);
    memset(wave_buffers,   0, sizeof(wave_buffers));
    memset(ApuEventQueue,  0, sizeof(ApuEventQueue));
    memset(nes_audio_ring, 0, sizeof(nes_audio_ring));
    memset(jpeg_buf,   0, sizeof(jpeg_buf));
    LOG_I("[NES] init: PSRAM memset done\r\n");

    jpeg_len = 0;
    jpeg_state = 0;
    audio_wr = audio_rd = 0;
    pad_state = 0;
    g_nes_running = 0;
}

void nes_bridge_set_pad(uint32_t pad)
{
    pad_state = pad;
}

int nes_bridge_running(void)
{
    return g_nes_running;
}

int nes_bridge_get_video(const uint8_t **out, uint32_t *len)
{
    if (jpeg_state == 1) {            /* filled & free to grab */
        *out = jpeg_buf;
        *len = jpeg_len;
        jpeg_state = 2;               /* USB now owns it */
        return 0;
    }
    return -1;                         /* no fresh frame yet */
}

/* Called by the USB video ISO callback once a full frame finished. */
void nes_bridge_release_video(void)
{
    if (jpeg_state == 2) {
        jpeg_state = 0;               /* free for re-encoding */
    }
}

uint32_t nes_bridge_get_audio(uint8_t *buf, uint32_t bytes)
{
    uint32_t frames = bytes / (AUDIO_CHANNELS * (AUDIO_BIT_DEPTH / 8));
    int16_t *pcm = (int16_t *)buf;
    uint32_t got = 0;

    taskENTER_CRITICAL();
    for (uint32_t i = 0; i < frames; i++) {
        if (audio_wr == audio_rd) break;   /* ring empty -> silence */
        pcm[i * AUDIO_CHANNELS + 0] = nes_audio_ring[audio_rd];
        pcm[i * AUDIO_CHANNELS + 1] = nes_audio_ring[audio_rd + 1];
        audio_rd = (audio_rd + AUDIO_CHANNELS) % AUDIO_RING_CAP;
        got++;
    }
    taskEXIT_CRITICAL();
    return got * AUDIO_CHANNELS * (AUDIO_BIT_DEPTH / 8);
}

/* ================================================================== *
 *  InfoNES portable API (signatures from InfoNES_System.h)
 * ================================================================== */

int InfoNES_ReadRom(const char *pszFileName)
{
    (void)pszFileName;

    if (g_nes_rom_len < 16) return -1;
    if (memcmp(g_nes_rom, "NES\x1a", 4) != 0) return -1;

    /* Parse the 16-byte iNES header into the core's NesHeader struct. */
    NesHeader.byID[0]     = g_nes_rom[0];
    NesHeader.byID[1]     = g_nes_rom[1];
    NesHeader.byID[2]     = g_nes_rom[2];
    NesHeader.byID[3]     = g_nes_rom[3];
    NesHeader.byRomSize   = g_nes_rom[4];
    NesHeader.byVRomSize  = g_nes_rom[5];
    NesHeader.byInfo1     = g_nes_rom[6];
    NesHeader.byInfo2     = g_nes_rom[7];
    memset(NesHeader.byReserve, 0, sizeof(NesHeader.byReserve));

    /* Point the core's ROM/VROM at the embedded image (PRG after header,
     * CHR after PRG). Cast away const: the core only reads these. */
    ROM  = (BYTE *)(g_nes_rom + 16);
    VROM = NULL;
    if (NesHeader.byVRomSize > 0) {
        VROM = (BYTE *)(g_nes_rom + 16 + (uint32_t)NesHeader.byRomSize * 0x4000UL);
    }
    return 0;
}

void InfoNES_ReleaseRom(void)
{
    /* ROM/VROM live in flash (const); nothing to free. */
    ROM  = NULL;
    VROM = NULL;
}

/* Called once per displayed frame (FrameCnt==0) when WorkFrame is stable. */
void InfoNES_LoadFrame(void)
{
    uint64_t prof_ts = prof_now();
    if (jpeg_state != 0) return;       /* busy (in flight) or already filled */

    const uint16_t *src = (const uint16_t *)WorkFrame;
    int w = NES_DISP_WIDTH, h = NES_DISP_HEIGHT;

    /* hw_mjpeg_encode does the optional 2x upscale internally (when
     * NES_FRAME_UPSCALE_2X is on) while converting RGB565 -> YUYV, so no
     * separate upscaled RGB565 scratch buffer is needed. */
    uint32_t n = hw_mjpeg_encode(src, w, h, SOFT_MJPEG_QUALITY,
                                 jpeg_buf, MJPEG_BUF_SZ);
    if (n > 0 && n < MJPEG_BUF_SZ - 2) {
        jpeg_len = n;
        jpeg_state = 1;                /* filled, free to grab */
        if (g_last) {
            g_gap_us += (prof_ts - g_last);
            g_gn++;
        }
        g_last = prof_ts;
        if (g_gn >= 60) {
            LOG_I("[PROF] gap=%luus  (sim+enc per frame)\r\n",
                  (unsigned long)(g_gap_us / g_gn / 320));
            g_gap_us = 0; g_gn = 0;
        }
    }

    /* FPS meter: one count per encoded frame, printed every ~1 s. Lets us
     * quantify the cost of NES_FRAME_UPSCALE_2X on the software encoder. */
    static uint32_t fps_cnt = 0;
    static uint32_t fps_t0 = 0;
    fps_cnt++;
    uint32_t now = xTaskGetTickCount();
    if (fps_t0 == 0) {
        fps_t0 = now;
    } else if (now - fps_t0 >= 1000) {
        LOG_I("[NES] enc fps=%lu (%dx%d q%d)\r\n",
              (unsigned long)(fps_cnt * 1000 / (now - fps_t0)), w, h,
              SOFT_MJPEG_QUALITY);
        fps_cnt = 0;
        fps_t0 = now;
    }
}

/* 5 APU waves (2 pulse, 1 triangle, 1 noise, 1 DPCM).
 * The InfoNES pAPU renders these UNIPOLAR (every sample >= 0):
 *   pulse   : 0 or 0x11(17) * (vol 0..15 + env 0..15)  -> 0..510 / channel
 *   triangle: 0..0xff(255) * (vol)                     -> 0..~7650
 *   noise   : 0..~510      * (vol)
 *   dpcm    : 1 + (dpcm<<1), dpcm 0..0x3f              -> 0..127
 * Summed raw range is ~0..9307, so the signal carries a large DC bias and
 * NO negative swing.  GAIN=400 (old) massively clipped and produced a
 * rectified (single-polarity) waveform -> harsh/distorted audio.
 *
 * Fix: (1) a one-pole DC blocker removes the bias so the output is properly
 * bipolar; (2) a modest, ear-tunable GAIN maps typical AC swing to a good
 * listening level; (3) hard clamp prevents overflow.  L/R duplicated (NES
 * is mono).  Tune NES_AUDIO_GAIN by ear (start 40; raise/lower ~x2 steps). */
#define NES_AUDIO_GAIN      40
#define NES_AUDIO_DC_SHIFT  8    /* DC-blocker pole: 1/256 ~ 30 Hz high-pass */

void InfoNES_SoundOutput(int samples, BYTE *w1, BYTE *w2, BYTE *w3, BYTE *w4, BYTE *w5)
{
    static int dc_est = 0;   /* slow mean of the unipolar sum (producer-only) */
    int16_t *dst = nes_audio_ring;

    taskENTER_CRITICAL();
    for (int i = 0; i < samples; i++) {
        int sum = (int)w1[i] + (int)w2[i] + (int)w3[i] + (int)w4[i] + (int)w5[i];
        int ac  = sum - dc_est;                 /* remove DC bias */
        dc_est += (sum - dc_est) >> NES_AUDIO_DC_SHIFT;   /* track the mean */
        int s = ac * NES_AUDIO_GAIN;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        int16_t v = (int16_t)s;
        dst[audio_wr]       = v;                /* L */
        dst[audio_wr + 1]   = v;                /* R */
        audio_wr = (audio_wr + AUDIO_CHANNELS) % AUDIO_RING_CAP;
    }
    taskEXIT_CRITICAL();
}

void InfoNES_PadState(DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem)
{
    *pdwPad1   = pad_state;
    *pdwPad2   = 0;
    *pdwSystem = 0;
}

void *InfoNES_MemoryCopy(void *dest, const void *src, int count)
{
    memcpy(dest, src, (size_t)count);
    return dest;
}

void *InfoNES_MemorySet(void *dest, int c, int count)
{
    memset(dest, c, (size_t)count);
    return dest;
}

void InfoNES_DebugPrint(char *szMsg)
{
    (void)szMsg;
}

int InfoNES_Menu(void)
{
    return 0;   /* no menu; run immediately */
}

/* Pace emulation to ~60 fps and yield to the USB task once per frame. */
void InfoNES_Wait(void)
{
    if (PPU_Scanline == 0) {
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

void InfoNES_SoundInit(void)
{
    /* Force NES APU to 44100 Hz / 735 samples per sync so the UAC endpoint
     * (AUDIO_SAMPLE_RATE) matches with no resampling. Called during Reset,
     * before pAPU uses ApuSamplesPerSync. */
    ApuQuality = 2;
    audio_wr = audio_rd = 0;
}

int InfoNES_SoundOpen(int samples_per_sync, int sample_rate)
{
    (void)samples_per_sync;
    (void)sample_rate;
    return 1;   /* we consume samples ourselves */
}

void InfoNES_SoundClose(void)
{
    /* nothing to close */
}

void InfoNES_MessageBox(char *pszMsg, ...)
{
    (void)pszMsg;
    /* variadic; ignore on embedded */
}

/* ================================================================== *
 *  Run the emulator (called from the NES task)
 * ================================================================== */
void nes_bridge_run(void)
{
    LOG_I("[NES] Load start\r\n");
    int ret = InfoNES_Load("");
    LOG_I("[NES] Load ret=%d\r\n", ret);
    if (ret != 0) {
        g_nes_running = 0;
        return;                 /* ROM failed: idle */
    }
    g_nes_running = 1;
    LOG_I("[NES] Main start\r\n");
    InfoNES_Main();            /* never returns (emulation loop) */
}
