/**
 * @file  usb_cdc.h
 * @brief USB CDC-ACM ("virtual COM port") on the native USB port.
 *
 * The PC sees /dev/ttyACMx (Linux) or COMx (Windows). Baud rate and the
 * other line settings mean nothing on USB and are only stored; except that
 * setting 1200 baud is the request to restart into the DFU bootloader
 * ("1200 bps touch", the same convention as Arduino boards).
 */
#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>

#define USB_CDC_TX_MAX  256U

void usb_cdc_init(void);                       /* start USB + enable its IRQ   */
int  usb_cdc_ready(void);                      /* configured by the host       */
int  usb_cdc_send(const uint8_t *data, uint32_t len);   /* 0 ok, -1 busy/too big */
int  usb_cdc_bootloader_requested(void);

/* Called from the USB interrupt with every received packet; defined by the
 * application. */
void usb_cdc_on_rx(const uint8_t *data, uint32_t len);

#endif /* USB_CDC_H */
