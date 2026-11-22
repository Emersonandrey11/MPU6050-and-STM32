/**
 * @file  usb.c
 * @brief Bare-metal USB full-speed device core (RM0008 "USB full-speed
 *        device interface", USB 2.0 specification chapter 9).
 *
 * How the STM32F103 USB peripheral works, in short:
 *
 *  - The serial engine handles the USB wire protocol (NRZI, bit stuffing,
 *    CRC, handshakes) by itself. Software only sees complete packets.
 *  - Packets live in a 512-byte Packet Memory Area (PMA) shared by the CPU
 *    and the USB engine. On the F103 the PMA is made of 16-bit words, each
 *    one at a 32-bit address: PMA byte offset x is at 0x40006000 + 2 * x.
 *  - A buffer descriptor table (BTABLE, here at PMA offset 0) tells the
 *    engine where each endpoint's TX/RX buffer is and how big it is.
 *  - Each endpoint register EPnR holds the endpoint's type, address and
 *    state: STAT_TX/STAT_RX = DISABLED, STALL, NAK or VALID. Software sets
 *    VALID when a buffer is ready; the hardware answers the host (ACK) and
 *    switches back to NAK by itself, then sets CTR_TX or CTR_RX
 *    ("correct transfer"). While a status is NAK, the hardware keeps telling
 *    the host "try again later", so software can take its time.
 *  - EPnR is tricky to write: STAT and DTOG bits TOGGLE when written with 1,
 *    CTR bits are CLEARED by writing 0. The ep_* helpers below build values
 *    that change only the intended bits.
 */
#include "usb.h"
#include "board.h"
#include "stm32f103_regs.h"

/* ---- Packet memory layout (byte offsets in the 512-byte PMA) ------------- */
#define PMA_BTABLE       0x000U        /* 8 endpoints x 8 bytes             */
#define PMA_EP0_RX       0x040U
#define PMA_EP0_TX       0x080U
#define PMA_FIRST_FREE   0x0C0U
#define PMA_SIZE         0x200U

#define PMA_WORD(off)    (*(volatile uint32_t *)(USB_PMA_BASE + 2UL * (uint32_t)(off)))
#define BT_ADDR_TX(ep)   PMA_WORD(PMA_BTABLE + 8U * (ep) + 0U)
#define BT_COUNT_TX(ep)  PMA_WORD(PMA_BTABLE + 8U * (ep) + 2U)
#define BT_ADDR_RX(ep)   PMA_WORD(PMA_BTABLE + 8U * (ep) + 4U)
#define BT_COUNT_RX(ep)  PMA_WORD(PMA_BTABLE + 8U * (ep) + 6U)

/* EPnR bits that are plain read/write: written back as read */
#define EP_RW_BITS       (USB_EP_EA | USB_EP_TYPE | USB_EP_KIND)

/* Standard requests (USB 2.0 table 9-4) and descriptor types (table 9-5) */
#define REQ_GET_STATUS        0x00U
#define REQ_CLEAR_FEATURE     0x01U
#define REQ_SET_FEATURE       0x03U
#define REQ_SET_ADDRESS       0x05U
#define REQ_GET_DESCRIPTOR    0x06U
#define REQ_GET_CONFIGURATION 0x08U
#define REQ_SET_CONFIGURATION 0x09U
#define REQ_GET_INTERFACE     0x0AU
#define REQ_SET_INTERFACE     0x0BU
#define DESC_DEVICE           0x01U
#define DESC_CONFIGURATION    0x02U
#define DESC_STRING           0x03U

typedef enum { CTRL_IDLE, CTRL_DATA_IN, CTRL_DATA_OUT, CTRL_STATUS_IN, CTRL_STATUS_OUT } ctrl_state_t;

static const usb_class_t *s_cls;
static usb_setup_t   s_req;
static ctrl_state_t  s_state;
static const uint8_t *s_in_ptr;
static uint16_t      s_in_left;
static uint8_t       s_in_zlp;          /* send a zero-length packet at the end */
static uint16_t      s_out_len, s_out_got;
static uint8_t       s_pending_addr;
static uint8_t       s_config;
static uint16_t      s_pma_next;
static uint16_t      s_tx_buf[8];       /* PMA offset of each endpoint's TX buffer */
static uint16_t      s_rx_buf[8];
static uint16_t      s_tx_size[8];
static uint8_t       s_ctrl_buf[USB_CTRL_BUF_SIZE];

/* ---- Small helpers ------------------------------------------------------- */

static void busy_wait_us(uint32_t us)
{
    /* ~4 cycles per loop; only used for start-up timings */
    for (volatile uint32_t n = us * (CORE_CLOCK_HZ / 4000000UL); n != 0U; n--) {
    }
}

static void pma_write(uint16_t off, const uint8_t *src, uint16_t len)
{
    for (uint16_t i = 0; i < len; i += 2U) {
        uint16_t v = src[i];
        if ((uint16_t)(i + 1U) < len) {
            v |= (uint16_t)(src[i + 1U] << 8);
        }
        PMA_WORD(off + i) = v;
    }
}

static void pma_read(uint16_t off, uint8_t *dst, uint16_t len)
{
    for (uint16_t i = 0; i < len; i += 2U) {
        uint16_t v = (uint16_t)PMA_WORD(off + i);
        dst[i] = (uint8_t)v;
        if ((uint16_t)(i + 1U) < len) {
            dst[i + 1U] = (uint8_t)(v >> 8);
        }
    }
}

/* COUNTn_RX upper bits: buffer size as BL_SIZE + NUM_BLOCK (RM0008, BTABLE) */
static uint16_t rx_size_field(uint16_t size)
{
    if (size > 62U) {
        return (uint16_t)(0x8000U | (((size / 32U) - 1U) << 10));   /* 32-byte blocks */
    }
    return (uint16_t)((size / 2U) << 10);                            /* 2-byte blocks  */
}

/* Set STAT_TX (or STAT_RX) to `stat` without touching anything else:
 * toggle bits get (current XOR wanted), CTR bits get 1 (= keep). */
static void ep_set_stat_tx(uint8_t ep, uint32_t stat)
{
    uint32_t v = USB_EPR(ep) & (EP_RW_BITS | USB_EP_STAT_TX);
    USB_EPR(ep) = (v ^ (stat << 4)) | USB_EP_CTR_RX | USB_EP_CTR_TX;
}

static void ep_set_stat_rx(uint8_t ep, uint32_t stat)
{
    uint32_t v = USB_EPR(ep) & (EP_RW_BITS | USB_EP_STAT_RX);
    USB_EPR(ep) = (v ^ (stat << 12)) | USB_EP_CTR_RX | USB_EP_CTR_TX;
}

static void ep_clear_ctr_rx(uint8_t ep)
{
    USB_EPR(ep) = (USB_EPR(ep) & EP_RW_BITS) | USB_EP_CTR_TX;      /* CTR_RX <- 0 */
}

static void ep_clear_ctr_tx(uint8_t ep)
{
    USB_EPR(ep) = (USB_EPR(ep) & EP_RW_BITS) | USB_EP_CTR_RX;      /* CTR_TX <- 0 */
}

/* Type + address, both directions NAK, both data toggles back to DATA0. */
static void ep_configure(uint8_t ep, uint32_t type)
{
    const uint32_t toggles = USB_EP_STAT_TX | USB_EP_STAT_RX | USB_EP_DTOG_TX | USB_EP_DTOG_RX;
    const uint32_t wanted  = (USB_STAT_NAK << 4) | (USB_STAT_NAK << 12);
    uint32_t v = USB_EPR(ep);
    USB_EPR(ep) = ep | type | ((v & toggles) ^ wanted);             /* CTR bits <- 0 */
}

/* Back to DATA0 after CLEAR_FEATURE(ENDPOINT_HALT) (USB 2.0, 9.4.5). */
static void ep_reset_toggle(uint8_t ep, int is_in)
{
    uint32_t bit = is_in ? USB_EP_DTOG_TX : USB_EP_DTOG_RX;
    uint32_t v = USB_EPR(ep);
    if (v & bit) {
        USB_EPR(ep) = (v & EP_RW_BITS) | bit | USB_EP_CTR_RX | USB_EP_CTR_TX;
    }
}

/* ---- Endpoints 1..7 ------------------------------------------------------- */

void usb_ep_open(uint8_t ep, uint32_t type, uint16_t tx_size, uint16_t rx_size)
{
    if ((ep == 0U) || (ep > 7U) || ((uint32_t)s_pma_next + tx_size + rx_size > PMA_SIZE)) {
        return;
    }
    s_tx_buf[ep] = s_pma_next;
    s_tx_size[ep] = tx_size;
    BT_ADDR_TX(ep) = s_pma_next;
    BT_COUNT_TX(ep) = 0U;
    s_pma_next = (uint16_t)(s_pma_next + tx_size);
    s_rx_buf[ep] = s_pma_next;
    BT_ADDR_RX(ep) = s_pma_next;
    BT_COUNT_RX(ep) = rx_size_field(rx_size);
    s_pma_next = (uint16_t)(s_pma_next + rx_size);
    ep_configure(ep, type);
}

int usb_ep_tx_busy(uint8_t ep)
{
    return ((USB_EPR(ep) & USB_EP_STAT_TX) >> 4) == USB_STAT_VALID;
}

int usb_ep_write(uint8_t ep, const uint8_t *data, uint16_t len)
{
    if (usb_ep_tx_busy(ep) || (len > s_tx_size[ep])) {
        return -1;
    }
    pma_write(s_tx_buf[ep], data, len);
    BT_COUNT_TX(ep) = len;
    ep_set_stat_tx(ep, USB_STAT_VALID);     /* the host may now read it */
    return 0;
}

uint16_t usb_ep_read(uint8_t ep, uint8_t *buf, uint16_t max)
{
    uint16_t n = (uint16_t)(BT_COUNT_RX(ep) & 0x3FFU);
    if (n > max) {
        n = max;
    }
    pma_read(s_rx_buf[ep], buf, n);
    return n;
}

void usb_ep_rx_enable(uint8_t ep)
{
    ep_set_stat_rx(ep, USB_STAT_VALID);
}

int usb_configured(void)
{
    return s_config != 0U;
}

/* ---- Endpoint 0: control transfers --------------------------------------- */

static void ep0_stall(void)
{
    ep_set_stat_tx(0U, USB_STAT_STALL);
    ep_set_stat_rx(0U, USB_STAT_STALL);   /* the next SETUP is accepted anyway */
    s_state = CTRL_IDLE;
}

static void ep0_send_chunk(void)
{
    uint16_t n = (s_in_left > USB_EP0_SIZE) ? USB_EP0_SIZE : s_in_left;
    pma_write(PMA_EP0_TX, s_in_ptr, n);
    BT_COUNT_TX(0U) = n;
    s_in_ptr += n;
    s_in_left = (uint16_t)(s_in_left - n);
    ep_set_stat_tx(0U, USB_STAT_VALID);
}

static void ep0_send_status(void)       /* zero-length IN packet */
{
    BT_COUNT_TX(0U) = 0U;
    ep_set_stat_tx(0U, USB_STAT_VALID);
    s_state = CTRL_STATUS_IN;
}

/* Build a string descriptor (UTF-16LE) from ASCII in s_ctrl_buf. */
static uint16_t string_descriptor(uint8_t index)
{
    char serial[25];
    const char *s;

    if (index == 0U) {                   /* supported languages: US English */
        s_ctrl_buf[0] = 4U; s_ctrl_buf[1] = DESC_STRING;
        s_ctrl_buf[2] = 0x09U; s_ctrl_buf[3] = 0x04U;
        return 4U;
    }
    if (index >= s_cls->num_strings) {
        return 0U;
    }
    s = s_cls->strings[index];
    if (s == 0) {                        /* serial number from the 96-bit unique ID */
        static const char HEX[] = "0123456789ABCDEF";
        for (uint32_t i = 0; i < 12U; i++) {
            uint8_t b = *(volatile const uint8_t *)(UNIQUE_ID_BASE + i);
            serial[2U * i] = HEX[b >> 4];
            serial[2U * i + 1U] = HEX[b & 0xFU];
        }
        serial[24] = '\0';
        s = serial;
    }
    uint16_t n = 2U;
    while ((*s != '\0') && (n + 2U <= 255U)) {
        s_ctrl_buf[n++] = (uint8_t)*s++;
        s_ctrl_buf[n++] = 0U;
    }
    s_ctrl_buf[0] = (uint8_t)n;
    s_ctrl_buf[1] = DESC_STRING;
    return n;
}

/* Standard requests. Returns USB_OK with *data / *len for an IN data stage
 * (len 0 = no data), or USB_STALL. */
static int standard_request(const uint8_t **data, uint16_t *len)
{
    uint8_t recipient = s_req.bmRequestType & 0x1FU;

    *len = 0U;
    switch (s_req.bRequest) {
    case REQ_GET_STATUS:
        s_ctrl_buf[0] = 0U;              /* bus powered, no remote wake-up, not halted */
        s_ctrl_buf[1] = 0U;
        *data = s_ctrl_buf;
        *len = 2U;
        return USB_OK;

    case REQ_CLEAR_FEATURE:
        if ((recipient == 2U) && (s_req.wValue == 0U) && ((s_req.wIndex & 0x0FU) != 0U)) {
            ep_reset_toggle((uint8_t)(s_req.wIndex & 0x0FU), (s_req.wIndex & 0x80U) != 0U);
        }
        return USB_OK;

    case REQ_SET_FEATURE:
        return USB_OK;

    case REQ_SET_ADDRESS:
        /* The new address applies only after the status stage (USB 2.0, 9.4.6) */
        s_pending_addr = (uint8_t)(s_req.wValue & 0x7FU);
        return USB_OK;

    case REQ_GET_DESCRIPTOR: {
        uint8_t type = (uint8_t)(s_req.wValue >> 8);
        if (type == DESC_DEVICE) {
            *data = s_cls->device_desc;
            *len = s_cls->device_desc[0];
        } else if (type == DESC_CONFIGURATION) {
            *data = s_cls->config_desc;
            *len = (uint16_t)(s_cls->config_desc[2] | (s_cls->config_desc[3] << 8));
        } else if (type == DESC_STRING) {
            *data = s_ctrl_buf;
            *len = string_descriptor((uint8_t)s_req.wValue);
            if (*len == 0U) {
                return USB_STALL;
            }
        } else {
            return USB_STALL;            /* e.g. device qualifier: full-speed only */
        }
        return USB_OK;
    }

    case REQ_GET_CONFIGURATION:
        s_ctrl_buf[0] = s_config;
        *data = s_ctrl_buf;
        *len = 1U;
        return USB_OK;

    case REQ_SET_CONFIGURATION:
        if (s_req.wValue > 1U) {
            return USB_STALL;
        }
        s_config = (uint8_t)s_req.wValue;
        if ((s_config != 0U) && (s_cls->configured != 0)) {
            s_pma_next = PMA_FIRST_FREE;
            s_cls->configured();
        }
        return USB_OK;

    case REQ_GET_INTERFACE:
        s_ctrl_buf[0] = 0U;
        *data = s_ctrl_buf;
        *len = 1U;
        return USB_OK;

    case REQ_SET_INTERFACE:
        return (s_req.wValue == 0U) ? USB_OK : USB_STALL;

    default:
        return USB_STALL;
    }
}

static void ep0_setup(void)
{
    uint8_t raw[8];
    const uint8_t *data = s_ctrl_buf;
    uint16_t len = 0U;
    int r;

    pma_read(PMA_EP0_RX, raw, 8U);
    s_req.bmRequestType = raw[0];
    s_req.bRequest      = raw[1];
    s_req.wValue        = (uint16_t)(raw[2] | (raw[3] << 8));
    s_req.wIndex        = (uint16_t)(raw[4] | (raw[5] << 8));
    s_req.wLength       = (uint16_t)(raw[6] | (raw[7] << 8));

    if ((s_req.bmRequestType & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD) {
        r = standard_request(&data, &len);
    } else if (s_cls->setup != 0) {
        r = s_cls->setup(&s_req, s_ctrl_buf, &len);
    } else {
        r = USB_STALL;
    }
    if (r != USB_OK) {
        ep0_stall();
        return;
    }

    if (s_req.bmRequestType & USB_REQ_DIR_IN) {
        if (len > s_req.wLength) {
            len = s_req.wLength;
        }
        /* A transfer shorter than wLength that ends on a full packet needs a
         * zero-length packet so the host knows it is complete. */
        s_in_zlp  = (uint8_t)((len < s_req.wLength) && ((len % USB_EP0_SIZE) == 0U));
        s_in_ptr  = data;
        s_in_left = len;
        s_state   = CTRL_DATA_IN;
        ep0_send_chunk();
        ep_set_stat_rx(0U, USB_STAT_VALID);   /* the host may end early with its status OUT */
    } else if (s_req.wLength != 0U) {
        if (s_req.wLength > sizeof s_ctrl_buf) {
            ep0_stall();
            return;
        }
        s_out_len = s_req.wLength;
        s_out_got = 0U;
        s_state   = CTRL_DATA_OUT;
        ep_set_stat_rx(0U, USB_STAT_VALID);
    } else {
        ep0_send_status();
    }
}

static void ep0_out(void)
{
    if (s_state == CTRL_DATA_OUT) {
        uint16_t n = (uint16_t)(BT_COUNT_RX(0U) & 0x3FFU);
        if (n > (uint16_t)(s_out_len - s_out_got)) {
            n = (uint16_t)(s_out_len - s_out_got);
        }
        pma_read(PMA_EP0_RX, &s_ctrl_buf[s_out_got], n);
        s_out_got = (uint16_t)(s_out_got + n);
        if (s_out_got < s_out_len) {
            ep_set_stat_rx(0U, USB_STAT_VALID);          /* more packets to come */
            return;
        }
        if ((s_cls->data_out == 0) || (s_cls->data_out(&s_req, s_ctrl_buf, s_out_len) != USB_OK)) {
            ep0_stall();
            return;
        }
        ep0_send_status();
    } else if ((s_state == CTRL_STATUS_OUT) || (s_state == CTRL_DATA_IN)) {
        /* Host acknowledged an IN transfer (possibly before reading all of
         * it, which USB allows): the request is complete. */
        ep_set_stat_tx(0U, USB_STAT_NAK);
        s_state = CTRL_IDLE;
        if (((s_req.bmRequestType & USB_REQ_TYPE_MASK) != USB_REQ_TYPE_STANDARD) && (s_cls->status_done != 0)) {
            s_cls->status_done(&s_req);
        }
    }
    ep_set_stat_rx(0U, USB_STAT_VALID);
}

static void ep0_in(void)
{
    if (s_state == CTRL_DATA_IN) {
        if (s_in_left > 0U) {
            ep0_send_chunk();
        } else if (s_in_zlp) {
            s_in_zlp = 0U;
            BT_COUNT_TX(0U) = 0U;
            ep_set_stat_tx(0U, USB_STAT_VALID);
        } else {
            s_state = CTRL_STATUS_OUT;                    /* wait for the host's ZLP */
        }
    } else if (s_state == CTRL_STATUS_IN) {
        s_state = CTRL_IDLE;
        if (s_pending_addr != 0U) {
            USB_DADDR = USB_DADDR_EF | s_pending_addr;
            s_pending_addr = 0U;
        }
        if (((s_req.bmRequestType & USB_REQ_TYPE_MASK) != USB_REQ_TYPE_STANDARD) && (s_cls->status_done != 0)) {
            s_cls->status_done(&s_req);
        }
        ep_set_stat_rx(0U, USB_STAT_VALID);
    }
}

/* ---- Bus events ------------------------------------------------------------ */

static void bus_reset(void)
{
    USB_BTABLE = PMA_BTABLE;
    BT_ADDR_RX(0U)  = PMA_EP0_RX;
    BT_COUNT_RX(0U) = rx_size_field(USB_EP0_SIZE);
    BT_ADDR_TX(0U)  = PMA_EP0_TX;
    BT_COUNT_TX(0U) = 0U;
    ep_configure(0U, USB_EP_TYPE_CONTROL);
    ep_set_stat_rx(0U, USB_STAT_VALID);
    for (uint8_t ep = 1U; ep < 8U; ep++) {
        uint32_t v = USB_EPR(ep);           /* disable: STAT and DTOG back to 0 */
        USB_EPR(ep) = ep | (v & (USB_EP_STAT_TX | USB_EP_STAT_RX | USB_EP_DTOG_TX | USB_EP_DTOG_RX));
    }
    s_pma_next = PMA_FIRST_FREE;
    s_config = 0U;
    s_state = CTRL_IDLE;
    s_pending_addr = 0U;
    USB_DADDR = USB_DADDR_EF;               /* address 0, function enabled */
    if (s_cls->bus_reset != 0) {
        s_cls->bus_reset();
    }
}

void usb_poll(void)
{
    uint32_t istr = USB_ISTR;

    if (istr & USB_ISTR_RESET) {
        USB_ISTR = (uint16_t)~USB_ISTR_RESET;          /* write 0 to clear, 1 to keep */
        bus_reset();
    }

    while ((istr = USB_ISTR) & USB_ISTR_CTR) {
        uint8_t  ep  = (uint8_t)(istr & USB_ISTR_EP_ID);
        uint32_t epr = USB_EPR(ep);

        if (epr & USB_EP_CTR_RX) {
            ep_clear_ctr_rx(ep);
            if (ep == 0U) {
                if (epr & USB_EP_SETUP) {
                    ep0_setup();
                } else {
                    ep0_out();
                }
            } else if (s_cls->ep_event != 0) {
                s_cls->ep_event(ep, 0);
            }
        }
        if (epr & USB_EP_CTR_TX) {
            ep_clear_ctr_tx(ep);
            if (ep == 0U) {
                ep0_in();
            } else if (s_cls->ep_event != 0) {
                s_cls->ep_event(ep, 1);
            }
        }
    }

    /* Flags we do not use (SOF, suspend, errors...) are set even when their
     * interrupt is masked; clear them so ISTR stays readable. */
    USB_ISTR = (uint16_t)(USB_ISTR_CTR | USB_ISTR_RESET);
}

/* Pull D+ low so the host sees a disconnect: the Blue Pill's D+ pull-up is
 * hard-wired, so this is the only way to make the PC enumerate again after
 * a reset (otherwise it still believes the old device is there). */
static void force_disconnect(uint32_t ms)
{
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN;
    GPIO_BRR(GPIOA_BASE) = 1UL << USB_DP_PIN;
    gpio_config(GPIOA_BASE, USB_DP_PIN, GPIO_MODE_OUT_PP_2MHZ);
    busy_wait_us(ms * 1000UL);
    gpio_config(GPIOA_BASE, USB_DP_PIN, GPIO_MODE_IN_FLOATING);
}

void usb_init(const usb_class_t *cls)
{
    s_cls = cls;
    force_disconnect(10U);

    /* Once the peripheral is enabled, PA11/PA12 belong to the USB
     * transceiver automatically: no alternate-function setup needed. */
    RCC_APB1ENR |= RCC_APB1ENR_USBEN;
    RCC_APB1RSTR |= RCC_APB1RSTR_USBRST;
    RCC_APB1RSTR &= ~RCC_APB1RSTR_USBRST;

    /* Start-up (RM0008): clear PDWN with FRES still set, wait t_STARTUP
     * (1 us max, datasheet) for the analog transceiver, then release FRES. */
    USB_CNTR = USB_CNTR_FRES;
    busy_wait_us(10U);
    USB_CNTR = 0U;
    USB_ISTR = 0U;
    USB_BTABLE = PMA_BTABLE;
    USB_CNTR = USB_CNTR_CTRM | USB_CNTR_RESETM;
}

void usb_stop(void)
{
    NVIC_ICER(USB_LP_CAN_RX0_IRQn / 32U) = 1UL << (USB_LP_CAN_RX0_IRQn % 32U);
    USB_CNTR = USB_CNTR_FRES | USB_CNTR_PDWN;
    RCC_APB1ENR &= ~RCC_APB1ENR_USBEN;
    force_disconnect(20U);
}

void usb_irq_enable(void)
{
    NVIC_ISER(USB_LP_CAN_RX0_IRQn / 32U) = 1UL << (USB_LP_CAN_RX0_IRQn % 32U);
}

/* Interrupt entry (vector "USB_LP_CAN_RX0", IRQ 20). Only enabled by the
 * application; the bootloader calls usb_poll() from its main loop. */
void USB_LP_CAN_RX0_IRQHandler(void)
{
    usb_poll();
}
