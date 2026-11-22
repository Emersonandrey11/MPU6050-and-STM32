/**
 * @file  usb_ids.h
 * @brief USB vendor/product IDs and the flash split shared by the app and the
 *        bootloader.
 *
 * WARNING: 0x1209:0x0001 and 0x1209:0x0002 are the pid.codes *test* PIDs.
 * pid.codes reserves them "for use in private testing"; they are not unique
 * and must not be used on a device that is redistributed, sold or
 * manufactured. For anything you hand to other people, request your own
 * free PID for an open-source project at https://pid.codes.
 */
#ifndef USB_IDS_H
#define USB_IDS_H

#define USB_VID              0x1209U   /* pid.codes (open-source hardware)     */
#define USB_PID_APP          0x0001U   /* test PID: application, USB serial    */
#define USB_PID_DFU          0x0002U   /* test PID: DFU bootloader             */

/* Flash split: 8 KB bootloader, then the application. */
#define BOOTLOADER_BASE      0x08000000UL
#define BOOTLOADER_SIZE      0x2000UL
#define APP_BASE             (BOOTLOADER_BASE + BOOTLOADER_SIZE)

/* Written to backup register BKP_DR1 by the application before a reset:
 * "stay in the bootloader". Backup registers survive a system reset. */
#define BOOT_REQUEST_MAGIC   0xB007U

#endif /* USB_IDS_H */
