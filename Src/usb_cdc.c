/**
 * @file  usb_cdc.c
 * @brief USB CDC-ACM class (USB CDC 1.2 + PSTN subclass specs) on usb.c.
 *
 * Interfaces and endpoints:
 *   interface 0  Communication (ACM)   EP3 IN  interrupt  (notifications, unused)
 *   interface 1  Data                  EP1 IN  bulk 64    device -> PC
 *                                      EP2 OUT bulk 64    PC -> device
 *
 * bInterfaceProtocol is 0 (no AT commands), so modem software such as
 * ModemManager has less reason to probe the port.
 */
#include "usb_cdc.h"
#include "board.h"
#include "stm32f103_regs.h"
#include "usb.h"
#include "usb_ids.h"

#define EP_DATA_IN    1U
#define EP_DATA_OUT   2U
#define EP_NOTIFY     3U
#define BULK_SIZE     64U

/* CDC class requests (PSTN 1.2, table 13) */
#define CDC_SET_LINE_CODING         0x20U
#define CDC_GET_LINE_CODING         0x21U
#define CDC_SET_CONTROL_LINE_STATE  0x22U
#define CDC_SEND_BREAK              0x23U

#define LO(x)  ((uint8_t)((x) & 0xFFU))
#define HI(x)  ((uint8_t)((x) >> 8))

static const uint8_t s_device_desc[18] = {
    18, 0x01,                 /* bLength, DEVICE                                   */
    0x00, 0x02,               /* bcdUSB 2.00                                       */
    0x02, 0x00, 0x00,         /* class CDC (interfaces say the rest)               */
    64,                       /* EP0 packet size                                   */
    LO(USB_VID), HI(USB_VID), LO(USB_PID_APP), HI(USB_PID_APP),
    0x00, 0x01,               /* bcdDevice 1.00                                    */
    1, 2, 3,                  /* strings: manufacturer, product, serial number     */
    1                         /* one configuration                                 */
};

#define CONFIG_LEN  67U
static const uint8_t s_config_desc[CONFIG_LEN] = {
    /* configuration */
    9, 0x02, LO(CONFIG_LEN), HI(CONFIG_LEN), 2, 1, 0, 0x80, 75,   /* 2 interfaces, bus powered, 150 mA */
    /* interface 0: communication, ACM */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x00, 0,
    5, 0x24, 0x00, 0x10, 0x01,        /* header functional descriptor, CDC 1.10    */
    5, 0x24, 0x01, 0x00, 1,           /* call management: none, data interface 1   */
    4, 0x24, 0x02, 0x02,              /* ACM: supports line coding + control lines */
    5, 0x24, 0x06, 0, 1,              /* union: master 0, slave 1                  */
    7, 0x05, 0x80 | EP_NOTIFY, 0x03, 16, 0, 255,    /* EP3 IN interrupt, 255 ms    */
    /* interface 1: data */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 0x05, EP_DATA_OUT, 0x02, BULK_SIZE, 0, 0,    /* EP2 OUT bulk                */
    7, 0x05, 0x80 | EP_DATA_IN, 0x02, BULK_SIZE, 0, 0,  /* EP1 IN bulk             */
};

static const char *const s_strings[] = {
    0,
    "MPU6050-and-STM32",
    "MPU6050 monitor (USB serial)",
    0,                        /* serial number: generated from the chip's unique ID */
};

static uint8_t           s_line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };   /* 115200 8N1 */
static volatile uint8_t  s_boot_request;
static volatile uint8_t  s_tx_busy;
static uint8_t           s_tx_buf[USB_CDC_TX_MAX];
static uint32_t          s_tx_pos, s_tx_len;
static uint8_t           s_tx_last;               /* size of the last packet sent */

static void tx_next(void)
{
    uint32_t n = s_tx_len - s_tx_pos;

    if (n == 0U) {
        if (s_tx_last == BULK_SIZE) {
            /* The transfer ended on a full packet: a zero-length packet
             * tells the host it is complete. */
            s_tx_last = 0U;
            (void)usb_ep_write(EP_DATA_IN, s_tx_buf, 0U);
            return;
        }
        s_tx_busy = 0U;
        return;
    }
    if (n > BULK_SIZE) {
        n = BULK_SIZE;
    }
    (void)usb_ep_write(EP_DATA_IN, &s_tx_buf[s_tx_pos], (uint16_t)n);
    s_tx_pos += n;
    s_tx_last = (uint8_t)n;
}

/* ---- usb_class_t callbacks (run inside the USB interrupt) ---------------- */

static int cdc_setup(const usb_setup_t *req, uint8_t *buf, uint16_t *len)
{
    if ((req->bmRequestType & USB_REQ_TYPE_MASK) != USB_REQ_TYPE_CLASS) {
        return USB_STALL;
    }
    switch (req->bRequest) {
    case CDC_SET_LINE_CODING:
        return (req->wLength == sizeof s_line_coding) ? USB_OK : USB_STALL;  /* data follows */
    case CDC_GET_LINE_CODING:
        for (uint32_t i = 0; i < sizeof s_line_coding; i++) {
            buf[i] = s_line_coding[i];
        }
        *len = sizeof s_line_coding;
        return USB_OK;
    case CDC_SET_CONTROL_LINE_STATE:      /* DTR/RTS: nothing to do */
    case CDC_SEND_BREAK:
        return USB_OK;
    default:
        return USB_STALL;
    }
}

static int cdc_data_out(const usb_setup_t *req, const uint8_t *buf, uint16_t len)
{
    if ((req->bRequest != CDC_SET_LINE_CODING) || (len != sizeof s_line_coding)) {
        return USB_STALL;
    }
    for (uint32_t i = 0; i < sizeof s_line_coding; i++) {
        s_line_coding[i] = buf[i];
    }
    uint32_t baud = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    if (baud == 1200U) {
        s_boot_request = 1U;
    }
    return USB_OK;
}

static void cdc_configured(void)
{
    usb_ep_open(EP_DATA_IN,  USB_EP_TYPE_BULK, BULK_SIZE, 0U);
    usb_ep_open(EP_DATA_OUT, USB_EP_TYPE_BULK, 0U, BULK_SIZE);
    usb_ep_open(EP_NOTIFY,   USB_EP_TYPE_INTERRUPT, 16U, 0U);
    usb_ep_rx_enable(EP_DATA_OUT);
    s_tx_busy = 0U;
}

static void cdc_ep_event(uint8_t ep, int is_in)
{
    if ((ep == EP_DATA_IN) && is_in) {
        tx_next();
    } else if ((ep == EP_DATA_OUT) && !is_in) {
        uint8_t buf[BULK_SIZE];
        uint16_t n = usb_ep_read(EP_DATA_OUT, buf, sizeof buf);
        usb_cdc_on_rx(buf, n);
        usb_ep_rx_enable(EP_DATA_OUT);
    }
}

static void cdc_bus_reset(void)
{
    s_tx_busy = 0U;
}

static const usb_class_t s_cdc = {
    .device_desc = s_device_desc,
    .config_desc = s_config_desc,
    .strings     = s_strings,
    .num_strings = (uint8_t)(sizeof s_strings / sizeof s_strings[0]),
    .setup       = cdc_setup,
    .data_out    = cdc_data_out,
    .status_done = 0,
    .configured  = cdc_configured,
    .ep_event    = cdc_ep_event,
    .bus_reset   = cdc_bus_reset,
};

/* ---- Public ---------------------------------------------------------------- */

void usb_cdc_init(void)
{
    usb_init(&s_cdc);
    usb_irq_enable();
}

int usb_cdc_ready(void)
{
    return usb_configured();
}

int usb_cdc_send(const uint8_t *data, uint32_t len)
{
    int result = -1;
    uint32_t primask = irq_save();      /* the USB interrupt also touches EP1 */

    if (usb_configured() && !s_tx_busy && (len <= sizeof s_tx_buf)) {
        for (uint32_t i = 0; i < len; i++) {
            s_tx_buf[i] = data[i];
        }
        s_tx_len = len;
        s_tx_pos = 0U;
        s_tx_busy = 1U;
        tx_next();
        result = 0;
    }
    irq_restore(primask);
    return result;
}

int usb_cdc_bootloader_requested(void)
{
    return s_boot_request != 0U;
}
