/**
 * @file  usb.h
 * @brief Bare-metal USB full-speed device core for the STM32F103.
 *
 * The core owns endpoint 0: it answers the standard requests (descriptors,
 * SET_ADDRESS, SET_CONFIGURATION, ...) and hands class requests to a class
 * driver (usb_cdc.c in the application, the DFU code in the bootloader).
 *
 * usb_poll() does all the work. The application calls it from the USB
 * interrupt (usb_irq_enable()); the bootloader simply calls it in a loop.
 */
#ifndef USB_H
#define USB_H

#include <stdint.h>

#define USB_EP0_SIZE       64U
#define USB_CTRL_BUF_SIZE  1024U   /* largest control data stage (DFU block) */

/* SETUP packet (USB 2.0 spec, chapter 9.3) */
typedef struct {
    uint8_t  bmRequestType;   /* bit 7: direction (1 = IN), bits 6..5: type, 4..0: recipient */
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

#define USB_REQ_DIR_IN        0x80U
#define USB_REQ_TYPE_MASK     0x60U
#define USB_REQ_TYPE_STANDARD 0x00U
#define USB_REQ_TYPE_CLASS    0x20U
#define USB_REQ_TYPE_VENDOR   0x40U

/* Return values of the class callbacks */
#define USB_OK     0
#define USB_STALL  (-1)

typedef struct {
    const uint8_t *device_desc;            /* 18 bytes                          */
    const uint8_t *config_desc;            /* wTotalLength bytes                */
    const char *const *strings;            /* ASCII; [0] unused; NULL = serial  */
    uint8_t num_strings;                   /* entries in strings[]              */

    /* Class/vendor SETUP. Device-to-host: fill buf, set *len.
     * Host-to-device with data: return USB_OK and the core receives
     * wLength bytes, then calls data_out(). Return USB_STALL to refuse. */
    int  (*setup)(const usb_setup_t *req, uint8_t *buf, uint16_t *len);
    int  (*data_out)(const usb_setup_t *req, const uint8_t *buf, uint16_t len);
    /* Called when the status stage of a class request has completed. */
    void (*status_done)(const usb_setup_t *req);
    /* SET_CONFIGURATION with a non-zero value: open the class endpoints. */
    void (*configured)(void);
    /* A transfer finished on endpoint 1..7 (is_in = 1: IN, 0: OUT). */
    void (*ep_event)(uint8_t ep, int is_in);
    /* The host sent a bus reset (optional). */
    void (*bus_reset)(void);
} usb_class_t;

void     usb_init(const usb_class_t *cls);
void     usb_stop(void);
void     usb_poll(void);
void     usb_irq_enable(void);
int      usb_configured(void);

/* Endpoints 1..7. Buffers are allocated in packet memory in call order and
 * released at every bus reset. type is one of USB_EP_TYPE_x (regs header). */
void     usb_ep_open(uint8_t ep, uint32_t type, uint16_t tx_size, uint16_t rx_size);
int      usb_ep_write(uint8_t ep, const uint8_t *data, uint16_t len);   /* -1: busy */
uint16_t usb_ep_read(uint8_t ep, uint8_t *buf, uint16_t max);
void     usb_ep_rx_enable(uint8_t ep);
int      usb_ep_tx_busy(uint8_t ep);

#endif /* USB_H */
