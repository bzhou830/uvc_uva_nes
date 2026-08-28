/*
 * main.c - UVC + UAC composite device entry point
 *
 * Builds as a Bouffalo SDK example. The device enumerates as a USB
 * webcam (UVC, static MJPEG) + microphone (UAC, 1 kHz tone).
 */
#include "board.h"
#include "bflb_uart.h"
#include "bflb_name.h"          /* BFLB_NAME_USB_V2 ("usb_v2") */
#include "usbd_core.h"

#include "FreeRTOS.h"
#include "task.h"

#include "log.h"
#include "uvc_uac.h"
#include "nes/infones_port.h"   /* InfoNES emulator bridge (Phase 2) */

#define DBG_TAG "UVC_UAC"

void shell_init_with_task(struct bflb_device_s *shell);
static void nes_task(void *param);   /* InfoNES emulator task (Phase 2) */

static void uvc_uac_task(void *param)
{
    (void)param;
    LOG_I("[UVCA] task start\r\n");

    struct bflb_device_s *usb = bflb_device_get_by_name(BFLB_NAME_USB_V2);
    LOG_I("[UVCA] usb dev=%p\r\n", (void *)usb);
    LOG_I("[UVCA] calling uvc_uac_init\r\n");
    uvc_uac_init(0, 0);
    LOG_I("UVC+UAC composite device initialized\r\n");

    while (1) {
        uvc_uac_poll(0);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void start_main(void *param)
{
    (void)param;
    LOG_I("[OS] start_main\r\n");

    struct bflb_device_s *uart0 = bflb_device_get_by_name("uart0");
    shell_init_with_task(uart0);

    xTaskCreate(uvc_uac_task, (char *)"uvc_uac", 4096, NULL, 5, NULL);

    /* Phase 2: InfoNES emulator task. Runs the embedded ROM (g_nes_rom[])
     * and produces MJPEG frames (InfoNES_LoadFrame -> jpeg_buf) + APU PCM
     * (nes_audio_ring) that the UVC/UAC task streams out. Priority 4 < USB
     * task (5) so USB transfers are never starved; InfoNES_Wait() yields
     * 16 ms/frame anyway. */
    xTaskCreate(nes_task, (char *)"nes", 8192, NULL, 4, NULL);

    vTaskDelete(NULL);
}

/* InfoNES emulator task: load the embedded ROM and run the emulation loop. */
static void nes_task(void *param)
{
    (void)param;
    LOG_I("[NES] task start\r\n");

    nes_bridge_init();
    LOG_I("[NES] bridge init done, starting emulator\r\n");
    nes_bridge_run();   /* InfoNES_Main() loop; returns only if ROM load fails */

    if (!nes_bridge_running()) {
        LOG_I("[NES] emulator did not start (ROM load failed)\r\n");
    }
    vTaskDelete(NULL);
}

int main(void)
{
    board_init();

    LOG_I("BL6x8 UVC+UAC composite demo\r\n");

#if defined(BOARD_USB_VIA_GPIO)
    board_usb_gpio_init();
#endif

    xTaskCreate(start_main, (char *)"start_task", 1024, NULL,
                configMAX_PRIORITIES - 1, NULL);

    LOG_I("[OS] start scheduler\r\n");
    vTaskStartScheduler();

    /* never reached */
    return 0;
}
