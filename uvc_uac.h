/*
 * uvc_uac.h - Composite UVC (webcam) + UAC (microphone) device
 *
 * BL616 / BL618 (Bouffalo Lab) acting as a USB Video + Audio source device.
 *
 * Phase 1: stream a MJPEG frame (UVC) + a 1 kHz test tone (UAC mic).
 *          BL616 has a dedicated HW MJPEG block (nes/hw_mjpeg.c): the NES
 *          RGB565 frame is converted to YUYV and hardware-encoded to baseline
 *          JPEG, so the frame path is fast enough to also run the optional
 *          2x upscale. Phase 2: frame/audio sources driven by InfoNES
 *          (PPU framebuffer -> MJPEG, APU -> PCM). See nes/infones_port.c.
 *
 * Resolution: VIDEO_WIDTH/HEIGHT default to 512x480 (NES 256x240 with the
 * NES_FRAME_UPSCALE_2X 2x nearest-neighbour upscale, see below). The UVC
 * descriptor derives its frame size from them, and the JPEG SOF0 matches, so
 * host and device always agree. The 256x240 RGB565 WorkFrame is small; the
 * 512x480 YUYV input buffer (491 KB) lives in PSRAM, not SRAM.
 * Audio is 44100 Hz to match the NES APU's native output (no resampling).
 */
#ifndef UVC_UAC_H
#define UVC_UAC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 *  Composite interface / endpoint assignment
 *  (single USB device, bDeviceClass = 0xEF with IAD)
 *
 *    UVC : iface 0 = VideoControl (VC)
 *          iface 1 = VideoStream  (VS, alt 1 = active, isochronous IN)
 *    UAC : iface 2 = AudioControl (AC)
 *          iface 3 = AudioStream  (mic, alt 1 = active, isochronous IN)
 *
 *  Endpoints (all Device -> Host, i.e. IN):
 *          0x81  UVC VideoStream isochronous IN
 *          0x82  UAC microphone   isochronous IN
 * ------------------------------------------------------------------ */
#define UVC_VC_INTERFACE        0
#define UVC_VS_INTERFACE        1
#define UAC_AC_INTERFACE        2
#define UAC_AS_MIC_INTERFACE    3

#define VIDEO_IN_EP             0x81
#define AUDIO_MIC_EP            0x82

/* ---- Video format: NES-native 256x240, optional 2x upscale to 512x480 ---- */
/* Optional 2x nearest-neighbour upscale BEFORE the MJPEG encode.
 *   0 = 256x240 @15fps
 *   1 = 512x480        : each NES pixel becomes a 2x2 block, so the host's
 *       4:2:2 chroma sampling (cosited, even columns) lands on the same NES
 *       pixel -> color fringing gone, edges crisp. With the BL616 HW MJPEG
 *       block this is essentially free (the encoder is hardware, not CPU),
 *       so 512x480 stays at 15fps. Default ON. */
#ifndef NES_FRAME_UPSCALE_2X
#define NES_FRAME_UPSCALE_2X  0
#endif

#if NES_FRAME_UPSCALE_2X
#define VIDEO_WIDTH            512
#define VIDEO_HEIGHT           480
#else
#define VIDEO_WIDTH            256
#define VIDEO_HEIGHT           240
#endif
#define VIDEO_FPS               30
#define VIDEO_INTERVAL_NS       (10000000UL / VIDEO_FPS)   /* 100ns units */

/* ---- Audio format (UAC mic; NES APU native 44100 Hz, 2ch, 16-bit) ---- */
#define AUDIO_SAMPLE_RATE       (44100UL)
#define AUDIO_CHANNELS          2
#define AUDIO_BIT_DEPTH         16          /* bits per sample */

/* ------------------------------------------------------------------ *
 *  Pluggable frame / audio source API
 *  Swap these implementations for Phase 2 (InfoNES).
 * ------------------------------------------------------------------ */

/* Fill *out_data / *out_len with the next video frame to stream.
 * Returns 0 on success, -1 if no frame is ready.
 * Phase 1: live-encodes a 256x240 test pattern to MJPEG (nes/soft_mjpeg.c).
 * Phase 2: encode the NES PPU framebuffer (nes_frame_rgb565) to MJPEG. */
int video_source_get_frame(const uint8_t **out_data, uint32_t *out_len);

/* Fill buf (little-endian PCM, AUDIO_CHANNELS * (AUDIO_BIT_DEPTH/8) bytes
 * per sample-frame) with the next block of audio samples.
 * Phase 1: 1 kHz sine tone.  Phase 2: NES APU PCM. */
void audio_source_fill(uint8_t *buf, uint32_t bytes);

/* ------------------------------------------------------------------ *
 *  Composite device init + polling
 * ------------------------------------------------------------------ */
void uvc_uac_init(uint8_t busid, uintptr_t reg_base);
void uvc_uac_poll(uint8_t busid);

#ifdef __cplusplus
}
#endif

#endif /* UVC_UAC_H */
