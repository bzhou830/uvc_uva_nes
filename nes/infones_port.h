/*
 * infones_port.h - InfoNES <-> BL6x8 UVC+UAC bridge (Phase 2)
 *
 * Implements the functions the InfoNES core (nes/infones_core sources) calls
 * to talk to the outside world, and exposes the bridge helpers that the
 * UVC/UAC sources (frame_audio_source.c) and main.c consume.
 *
 *   InfoNES_LoadFrame()   <- PPU finished a frame -> encode WorkFrame -> MJPEG
 *   InfoNES_SoundOutput() <- 5 APU waves         -> mix -> audio ring
 *   InfoNES_PadState()    <- joypad (queue)
 *   InfoNES_ReadRom()     <- embedded g_nes_rom[] (flash/XIP)
 *   InfoNES memory / wait / menu / message-box / debug-print / sound helpers
 *
 * The core function prototypes come from InfoNES_System.h; do NOT redeclare
 * them here.
 */
#ifndef INFO_NES_PORT_H
#define INFO_NES_PORT_H

#include <stdint.h>

/* ---- Bridge helpers (used by UVC/UAC sources + main.c) ---- */
void nes_bridge_init(void);
void nes_bridge_run(void);                  /* runs the InfoNES loop forever */
int  nes_bridge_running(void);             /* 1 once the ROM loaded OK */
int  nes_bridge_get_video(const uint8_t **out, uint32_t *len);
uint32_t nes_bridge_get_audio(uint8_t *buf, uint32_t bytes);
void nes_bridge_set_pad(uint32_t pad);
void nes_bridge_release_video(void);        /* USB ISO callback: a frame finished */

#endif /* INFO_NES_PORT_H */
