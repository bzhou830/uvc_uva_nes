/*
 * usb_composite.c - Merged UVC (MJPEG camera) + UAC (microphone) descriptors
 *
 * Both class drivers are native to the SDK's bundled CherryUSB
 * (components/usb/cherryusb/class/video and .../class/audio).
 *
 * The two templates each hard-code interface 0 and overlap endpoint 0x81,
 * so this file re-numbers the interfaces and endpoints to build ONE
 * composite config descriptor:
 *   UVC  -> iface 0 (VC) + iface 1 (VS, iso IN 0x81)
 *   UAC  -> iface 2 (AC) + iface 3 (AS mic, iso IN 0x82)
 */
#include "usbd_core.h"
#include "usbd_video.h"
#include "usbd_audio.h"
#include "usbd_hid.h"

#include "log.h"
#include "uvc_uac.h"
#include "nes/infones_port.h"   /* nes_bridge_release_video() */

/* ===================== Endpoint / format constants ===================== */

#ifdef CONFIG_USB_HS
#define MAX_PAYLOAD_SIZE       512
#define VIDEO_PACKET_SIZE      (unsigned int)(((MAX_PAYLOAD_SIZE / 1)) | (0x00 << 11))
#define AUDIO_EP_INTERVAL      0x04
#else
#define MAX_PAYLOAD_SIZE       510
#define VIDEO_PACKET_SIZE      (unsigned int)(((MAX_PAYLOAD_SIZE / 1)) | (0x00 << 11))
#define AUDIO_EP_INTERVAL      0x01
#endif

/* ---- UVC ---- */
#define VIDEO_MIN_BIT_RATE     (unsigned long)(VIDEO_WIDTH * VIDEO_HEIGHT * 16 * VIDEO_FPS)
#define VIDEO_MAX_BIT_RATE     (unsigned long)(VIDEO_WIDTH * VIDEO_HEIGHT * 16 * VIDEO_FPS)
#define VIDEO_MAX_FRAME_SIZE   (unsigned long)(VIDEO_WIDTH * VIDEO_HEIGHT * 2)

#define VS_HEADER_SIZ          (unsigned int)(VIDEO_SIZEOF_VS_INPUT_HEADER_DESC(1, 1) + \
                                              VIDEO_SIZEOF_VS_FORMAT_MJPEG_DESC + \
                                              VIDEO_SIZEOF_VS_FRAME_MJPEG_DESC(1))

/* ---- UAC mic-only ---- */
#define AUDIO_IN_CHANNEL_NUM   2
#if AUDIO_IN_CHANNEL_NUM == 1
#define AUDIO_MIC_CTRL         0x03, 0x03
#define AUDIO_MIC_CH_ENABLE    0x0001
#elif AUDIO_IN_CHANNEL_NUM == 2
#define AUDIO_MIC_CTRL         0x03, 0x03, 0x03
#define AUDIO_MIC_CH_ENABLE    0x0003
#endif
#define AUDIO_MIC_FRAME_SIZE_BYTE  2u
#define AUDIO_MIC_RESOLUTION_BIT  16u
#define AUDIO_IN_PACKET  ((uint32_t)((AUDIO_SAMPLE_RATE * AUDIO_MIC_FRAME_SIZE_BYTE * AUDIO_IN_CHANNEL_NUM) / 1000))

/* AudioControl interface total length (IAD + AC iface + AC header + units).
 * Mirrors the layout produced by AUDIO_AC_DESCRIPTOR_INIT. */
#define AUDIO_AC_SIZ   (AUDIO_SIZEOF_AC_HEADER_DESC(1) + \
                        AUDIO_SIZEOF_AC_INPUT_TERMINAL_DESC + \
                        AUDIO_SIZEOF_AC_FEATURE_UNIT_DESC(AUDIO_IN_CHANNEL_NUM, 1) + \
                        AUDIO_SIZEOF_AC_OUTPUT_TERMINAL_DESC)

/* ===================== Descriptor size computation ===================== */
/* UVC part (excludes the 9-byte config header): VC + VS(iface/alt0) + VS header + VS(iface/alt1) + EP */
#define UVC_DESC_PART  (VIDEO_VC_NOEP_DESCRIPTOR_LEN + 9 + VS_HEADER_SIZ + 9 + 7)
/* UAC part: AC(IAD+iface+header+ba) + input term + feature unit + output term + AS(mic) */
#define UAC_DESC_PART  (AUDIO_AC_DESCRIPTOR_LEN(1) \
                        + AUDIO_SIZEOF_AC_INPUT_TERMINAL_DESC \
                        + AUDIO_SIZEOF_AC_FEATURE_UNIT_DESC(AUDIO_IN_CHANNEL_NUM, 1) \
                        + AUDIO_SIZEOF_AC_OUTPUT_TERMINAL_DESC \
                        + AUDIO_AS_DESCRIPTOR_LEN(1))
#define HID_DESC_PART          (9 + 9 + 7)   /* iface(9) + HID class desc(9) + OUT EP(7) */
#define USB_COMPOSITE_DESC_SIZ  (unsigned long)(9 + UVC_DESC_PART + UAC_DESC_PART + HID_DESC_PART)

#define USBD_VID        0xABCD
#define USBD_PID        0x1234
#define USBD_MAX_POWER  100

/* ---- HID (NES pad input receiver, iface 4) ---- */
#define HID_OUT_EP              0x03    /* interrupt OUT, free (VC=0x81, AC=0x82) */
/* 1-byte Output report, Report ID 1, vendor usage page 0xFF00 */
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
#define HID_REPORT_DESC_LEN    (sizeof(hid_report_desc))

/* ===================== Descriptors ===================== */
static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xef, 0x02, 0x01, USBD_VID, USBD_PID, 0x0001, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_COMPOSITE_DESC_SIZ, 0x05, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),

    /* ----------------------- UVC (iface 0 VC, iface 1 VS) ----------------------- */
    VIDEO_VC_NOEP_DESCRIPTOR_INIT(0x00, 0x00, 0x0100, VIDEO_VC_TERMINAL_LEN, 48000000, 0x02),
    VIDEO_VS_DESCRIPTOR_INIT(0x01, 0x00, 0x00),
    VIDEO_VS_INPUT_HEADER_DESCRIPTOR_INIT(0x01, VS_HEADER_SIZ, VIDEO_IN_EP, 0x00),
    VIDEO_VS_FORMAT_MJPEG_DESCRIPTOR_INIT(0x01, 0x01),
    VIDEO_VS_FRAME_MJPEG_DESCRIPTOR_INIT(0x01, VIDEO_WIDTH, VIDEO_HEIGHT,
                                         VIDEO_MIN_BIT_RATE, VIDEO_MAX_BIT_RATE, VIDEO_MAX_FRAME_SIZE,
                                         DBVAL(VIDEO_INTERVAL_NS), 0x01, DBVAL(VIDEO_INTERVAL_NS)),
    VIDEO_VS_DESCRIPTOR_INIT(0x01, 0x01, 0x01),
    USB_ENDPOINT_DESCRIPTOR_INIT(VIDEO_IN_EP, 0x05, VIDEO_PACKET_SIZE, 0x01),

    /* ----------------------- UAC mic (iface 2 AC, iface 3 AS) ----------------------- */
    AUDIO_AC_DESCRIPTOR_INIT(0x02, 0x02, AUDIO_AC_SIZ, 0x02, 0x03),
    AUDIO_AC_INPUT_TERMINAL_DESCRIPTOR_INIT(0x01, AUDIO_INTERM_MIC, AUDIO_IN_CHANNEL_NUM, AUDIO_MIC_CH_ENABLE),
    AUDIO_AC_FEATURE_UNIT_DESCRIPTOR_INIT(0x02, 0x01, 0x01, AUDIO_MIC_CTRL),
    AUDIO_AC_OUTPUT_TERMINAL_DESCRIPTOR_INIT(0x03, AUDIO_TERMINAL_STREAMING, 0x02),
    AUDIO_AS_DESCRIPTOR_INIT(0x03, 0x03, AUDIO_IN_CHANNEL_NUM,
                             AUDIO_MIC_FRAME_SIZE_BYTE, AUDIO_MIC_RESOLUTION_BIT,
                             AUDIO_MIC_EP, 0x05, AUDIO_IN_PACKET, AUDIO_EP_INTERVAL,
                             AUDIO_SAMPLE_FREQ_3B(AUDIO_SAMPLE_RATE)),

    /* ----------------------- HID (iface 4): NES pad input from host ----------------------- */
    /* 1-byte Output report (Report ID 1) = NES button bitmask; host writes it over the
       interrupt OUT endpoint 0x03, or via Set_Report control transfer. */
    USB_INTERFACE_DESCRIPTOR_INIT(0x04, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00),
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, WBVAL(HID_REPORT_DESC_LEN),
    USB_ENDPOINT_DESCRIPTOR_INIT(HID_OUT_EP, 0x03, 64, 0x01),
};

static const uint8_t device_quality_descriptor[] = {
    0x0a,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },   /* Langid */
    "Bouffalo",                    /* Manufacturer */
    "BL6x8_UVC_UAC_DEMO",          /* Product */
    "2024082701",                  /* Serial Number */
};

static const uint8_t *device_descriptor_callback(uint8_t speed)        { return device_descriptor; }
static const uint8_t *config_descriptor_callback(uint8_t speed)        { return config_descriptor; }
static const uint8_t *device_quality_descriptor_callback(uint8_t speed) { return device_quality_descriptor; }
static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    if (index > 3) return NULL;
    return string_descriptors[index];
}

static const struct usb_descriptor composite_descriptor = {
    .device_descriptor_callback         = device_descriptor_callback,
    .config_descriptor_callback         = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback         = string_descriptor_callback,
};

/* ===================== Runtime state / buffers ===================== */
static volatile bool video_streaming = false;
static volatile bool video_busy      = false;
static volatile bool audio_streaming = false;
static volatile bool audio_busy      = false;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t video_packet_buf[MAX_PAYLOAD_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t audio_write_buf[AUDIO_IN_PACKET];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_out_buf[64];

/* ===================== Callbacks ===================== */
static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
    switch (event) {
    case USBD_EVENT_RESET:
        video_streaming = false;
        audio_streaming = false;
        video_busy = false;
        audio_busy = false;
        break;
    case USBD_EVENT_CONFIGURED:
        /* arm the HID pad OUT endpoint once the host sets the configuration */
        usbd_ep_start_read(busid, HID_OUT_EP, hid_out_buf, sizeof(hid_out_buf));
        break;
    default:
        break;
    }
}

/* --- UVC --- */
void usbd_video_open(uint8_t busid, uint8_t intf)
{
    (void)busid;
    if (intf == UVC_VS_INTERFACE) {
        video_streaming = true;
        video_busy = false;
    }
}
void usbd_video_close(uint8_t busid, uint8_t intf)
{
    (void)busid;
    if (intf == UVC_VS_INTERFACE) {
        video_streaming = false;
        video_busy = false;
    }
}
void usbd_video_iso_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep;
    if (nbytes) {
        if (usbd_video_stream_split_transfer(busid, ep)) {
            video_busy = false;          /* one full frame finished */
            nes_bridge_release_video();  /* free MJPEG buffer for re-encode */
        }
    }
}

/* --- UAC mic --- */
void usbd_audio_open(uint8_t busid, uint8_t intf)
{
    (void)busid;
    if (intf == UAC_AS_MIC_INTERFACE) {
        audio_streaming = true;
        audio_busy = false;
    }
}
void usbd_audio_close(uint8_t busid, uint8_t intf)
{
    (void)busid;
    if (intf == UAC_AS_MIC_INTERFACE) {
        audio_streaming = false;
        audio_busy = false;
    }
}
void usbd_audio_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep; (void)nbytes;
    audio_busy = false;
}

/* --- HID pad receiver (iface 4, OUT EP 0x03) --- */
static void hid_out_ep_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep;
    /* packet layout: [report_id=1][nes_pad_byte]; only act on a full report */
    if (nbytes >= 2) {
        nes_bridge_set_pad(hid_out_buf[1]);
    }
    /* re-arm the OUT endpoint for the next packet */
    usbd_ep_start_read(busid, HID_OUT_EP, hid_out_buf, sizeof(hid_out_buf));
}
static struct usbd_endpoint hid_out_ep = {
    .ep_cb   = hid_out_ep_cb,
    .ep_addr = HID_OUT_EP,
};
static struct usbd_interface hid_intf;   /* HID (iface 4) */

/* HID Set_Report (control path) fallback: host may send the NES pad byte via
   EP0 Set_Report instead of the interrupt OUT endpoint. */
void usbd_hid_set_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t *report, uint32_t report_len)
{
    (void)busid; (void)intf; (void)report_id; (void)report_type;
    if (report_len >= 1) {
        nes_bridge_set_pad(report[0]);
    }
}

/* ===================== Endpoint / interface registration ===================== */
static struct usbd_endpoint video_in_ep = {
    .ep_cb   = usbd_video_iso_callback,
    .ep_addr = VIDEO_IN_EP,
};
static struct usbd_endpoint audio_in_ep = {
    .ep_cb   = usbd_audio_in_callback,
    .ep_addr = AUDIO_MIC_EP,
};

static struct usbd_interface video_intf0;   /* VC  (iface 0) */
static struct usbd_interface video_intf1;   /* VS  (iface 1) */
static struct usbd_interface audio_intf0;   /* AC  (iface 2) */
static struct usbd_interface audio_intf1;   /* AS  (iface 3) */

static struct audio_entity_info audio_entity_table[] = {
    { .bEntityId = 0x02, .bDescriptorSubtype = AUDIO_CONTROL_FEATURE_UNIT, .ep = AUDIO_MIC_EP },
};

void uvc_uac_init(uint8_t busid, uintptr_t reg_base)
{
    LOG_I("[UVCA] uvc_uac_init enter, busid=%d reg_base=%p\r\n", busid, (void *)reg_base);

    usbd_desc_register(busid, &composite_descriptor);
    LOG_I("[UVCA] desc_register ok\r\n");

    /* Order MUST match the descriptor: VC(0), VS(1), AC(2), AS mic(3) */
    usbd_add_interface(busid, usbd_video_init_intf(busid, &video_intf0, VIDEO_INTERVAL_NS, VIDEO_MAX_FRAME_SIZE, MAX_PAYLOAD_SIZE));
    LOG_I("[UVCA] video intf0 (VC) ok\r\n");
    usbd_add_interface(busid, usbd_video_init_intf(busid, &video_intf1, VIDEO_INTERVAL_NS, VIDEO_MAX_FRAME_SIZE, MAX_PAYLOAD_SIZE));
    LOG_I("[UVCA] video intf1 (VS) ok\r\n");
    usbd_add_interface(busid, usbd_audio_init_intf(busid, &audio_intf0, 0x0100, audio_entity_table, 1));
    LOG_I("[UVCA] audio intf0 (AC) ok\r\n");
    usbd_add_interface(busid, usbd_audio_init_intf(busid, &audio_intf1, 0x0100, audio_entity_table, 1));
    LOG_I("[UVCA] audio intf1 (AS) ok\r\n");

    usbd_add_endpoint(busid, &video_in_ep);
    LOG_I("[UVCA] video ep 0x81 ok\r\n");
    usbd_add_endpoint(busid, &audio_in_ep);
    LOG_I("[UVCA] audio ep 0x82 ok\r\n");

    /* HID pad receiver: register interface (report descriptor) + OUT endpoint */
    usbd_add_interface(busid, usbd_hid_init_intf(busid, &hid_intf, hid_report_desc, HID_REPORT_DESC_LEN));
    usbd_add_endpoint(busid, &hid_out_ep);
    LOG_I("[UVCA] hid intf4 (pad) + ep 0x03 ok\r\n");

    LOG_I("[UVCA] calling usbd_initialize\r\n");
    usbd_initialize(busid, reg_base, usbd_event_handler);
    LOG_I("[UVCA] usbd_initialize done\r\n");
}

/* ===================== Per-tick polling (from RTOS task) ===================== */
void uvc_uac_poll(uint8_t busid)
{
    /* Video: stream the current frame when the previous one finished */
    if (video_streaming && !video_busy) {
        const uint8_t *frame;
        uint32_t len;
        if (video_source_get_frame(&frame, &len) == 0 && len > 0) {
            video_busy = true;
            usbd_video_stream_start_write(busid, VIDEO_IN_EP, video_packet_buf,
                                          (uint8_t *)frame, len, true);
        }
    }

    /* Audio: feed the next PCM block */
    if (audio_streaming && !audio_busy) {
        audio_source_fill(audio_write_buf, AUDIO_IN_PACKET);
        audio_busy = true;
        usbd_ep_start_write(busid, AUDIO_MIC_EP, audio_write_buf, AUDIO_IN_PACKET);
    }
}
