/*
 * usb_storage — USB Mass Storage (Bulk-Only Transport) .cctk module.
 *
 * Binds mass-storage interfaces that speak Bulk-Only Transport (class 0x08,
 * protocol 0x50) and exposes the device as a 512-byte-sector block device via
 * register_blkdev().  The kernel's partition layer then probes MBR/GPT and the
 * filesystem modules (fat32.cctk / ext4.cctk) mount it.
 *
 * BOT transaction = CBW (31-byte command block) -> optional data stage -> CSW
 * (13-byte status).  SCSI commands used: TEST UNIT READY, REQUEST SENSE,
 * INQUIRY, READ CAPACITY(10), READ(10), WRITE(10).
 */

#include <stdint.h>
#include <stddef.h>
#include "usb.h"
#include "sync.h"

/* Kernel entry points resolved through the ksym table. */
extern int   usb_driver_register(usb_driver_t *drv);
extern int   usb_driver_unregister(usb_driver_t *drv);
extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *dst, const void *src, unsigned int n);
extern void  printk(const char *fmt, ...);
extern void  ktime_busy_wait_us(uint64_t us);
extern void  mutex_init(mutex_t *m);
extern void  mutex_lock(mutex_t *m);
extern void  mutex_unlock(mutex_t *m);
extern int   register_blkdev(const char *name, uint32_t max_lba,
                             void (*read_sector)(uint32_t lba, uint8_t *buf),
                             void (*write_sector)(uint32_t lba, uint8_t *buf));
extern void  unregister_blkdev(const char *name);

#define STORAGE_DISK_NAME "usbsd"
#define SECTOR_SIZE       512

/* usb.h has no recipient-endpoint request-type macro. */
#define USB_RT_ENDPOINT 0x02

/* Bulk-Only Transport class requests (USB MSC rev 1.0). */
#define MSC_REQ_RESET        0xFF
#define MSC_REQ_GET_MAX_LUN  0xFE

#define CBW_SIGNATURE 0x43425355u   /* "USBC" */
#define CSW_SIGNATURE 0x53425355u   /* "USBS" */

#define BOT_DIR_IN   0x80
#define BOT_DIR_OUT  0x00

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_len;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cb_len;
    uint8_t  cdb[16];
} bot_cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;
} bot_csw_t;

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A

#define BOT_TIMEOUT_MS 5000

typedef struct {
    usb_device_t *dev;
    uint8_t       bulk_in;
    uint8_t       bulk_out;
    uint8_t       lun;
    uint32_t      tag;
    uint32_t      block_bytes;   /* device block size (usually 512) */
    uint32_t      max_lba;       /* capacity in 512-byte sectors */
    uint8_t      *block_buf;     /* scratch for read-modify-write if != 512 */
    int           present;
    mutex_t       lock;
} usb_storage_t;

/* One device is supported (enough for a single flash drive). */
static usb_storage_t g;

static int msc_bulk(usb_storage_t *s, uint8_t ep, uint8_t dir,
                    void *buf, uint32_t len)
{
    int rc;
    if (len == 0)
        return 0;
    rc = s->dev->hc->bulk_transfer(s->dev->hc, s->dev, ep, dir, buf,
                                   (uint16_t)len, BOT_TIMEOUT_MS);
    return (rc == (int)len) ? 0 : -1;
}

/* One BOT command.  Returns 0 on success, 1 when the command failed (CSW
 * status != 0, i.e. a sense condition) and -1 on a transport error. */
static int msc_command(usb_storage_t *s, const uint8_t *cdb, uint8_t cdblen,
                       void *data, uint32_t datalen, uint8_t dir,
                       uint8_t *out_status)
{
    bot_cbw_t cbw;
    bot_csw_t csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag       = ++s->tag;
    cbw.data_len  = datalen;
    cbw.flags     = dir;
    cbw.lun       = s->lun;
    cbw.cb_len    = cdblen;
    memcpy(cbw.cdb, cdb, cdblen);

    if (msc_bulk(s, s->bulk_out, USB_DIR_OUT, &cbw, sizeof(cbw)) < 0)
        return -1;

    if (datalen) {
        if (dir == BOT_DIR_IN) {
            if (msc_bulk(s, s->bulk_in, USB_DIR_IN, data, datalen) < 0)
                return -1;
        } else {
            if (msc_bulk(s, s->bulk_out, USB_DIR_OUT, data, datalen) < 0)
                return -1;
        }
    }

    memset(&csw, 0, sizeof(csw));
    if (msc_bulk(s, s->bulk_in, USB_DIR_IN, &csw, sizeof(csw)) < 0)
        return -1;

    if (csw.signature != CSW_SIGNATURE || csw.tag != cbw.tag)
        return -1;
    if (out_status)
        *out_status = csw.status;
    return (csw.status == 0) ? 0 : 1;
}

static void msc_reset(usb_storage_t *s) {
    usb_device_t *dev = s->dev;
    usb_setup_pkt_t sp;

    memset(&sp, 0, sizeof(sp));
    sp.bmRequestType = USB_RT_HOST_TO_DEV | USB_RT_CLASS | USB_RT_INTERFACE;
    sp.bRequest      = MSC_REQ_RESET;
    dev->hc->control_transfer(dev->hc, dev, &sp, NULL, 0);

    /* Clear any endpoint halt left over from a previous stack. */
    sp.bmRequestType = USB_RT_HOST_TO_DEV | USB_RT_ENDPOINT;
    sp.bRequest      = USB_REQ_CLEAR_FEATURE;
    sp.wValue        = 0;                        /* ENDPOINT_HALT */
    sp.wIndex        = (uint16_t)(s->bulk_in | 0x80);
    dev->hc->control_transfer(dev->hc, dev, &sp, NULL, 0);
    sp.wIndex        = (uint16_t)(s->bulk_out & 0x0F);
    dev->hc->control_transfer(dev->hc, dev, &sp, NULL, 0);
}

static void msc_get_max_lun(usb_storage_t *s) {
    usb_setup_pkt_t sp;
    uint8_t lun = 0;

    memset(&sp, 0, sizeof(sp));
    sp.bmRequestType = USB_RT_DEV_TO_HOST | USB_RT_CLASS | USB_RT_INTERFACE;
    sp.bRequest      = MSC_REQ_GET_MAX_LUN;
    sp.wLength       = 1;
    /* A device that does not implement it STALLs, which is legal: use LUN 0. */
    if (s->dev->hc->control_transfer(s->dev->hc, s->dev, &sp, &lun, 1) != 0)
        lun = 0;
    s->lun = lun & 0x0F;
}

static int msc_request_sense(usb_storage_t *s) {
    uint8_t cdb[6];
    uint8_t sense[18];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = sizeof(sense);
    return msc_command(s, cdb, 6, sense, sizeof(sense), BOT_DIR_IN, NULL);
}

static int msc_unit_ready(usb_storage_t *s) {
    uint8_t cdb[6];
    uint8_t st = 0;
    int rc;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_TEST_UNIT_READY;
    rc = msc_command(s, cdb, 6, NULL, 0, BOT_DIR_IN, &st);
    /* A failed command is usually UNIT ATTENTION right after plug-in; clear it
     * with REQUEST SENSE so the next attempt can succeed. */
    if (rc == 1)
        msc_request_sense(s);
    return rc;
}

static int msc_read_capacity(usb_storage_t *s, uint32_t *last_lba, uint32_t *bs) {
    uint8_t cdb[10];
    uint8_t buf[8];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_CAPACITY10;
    if (msc_command(s, cdb, 10, buf, sizeof(buf), BOT_DIR_IN, NULL) != 0)
        return -1;
    *last_lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
    *bs       = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                ((uint32_t)buf[6] << 8)  | (uint32_t)buf[7];
    return 0;
}

/* READ(10)/WRITE(10) of `count` device blocks (block_bytes each). */
static int msc_rw(usb_storage_t *s, uint32_t block, uint32_t count,
                  void *buf, int write)
{
    uint8_t cdb[10];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
    cdb[2] = (block >> 24) & 0xFF;
    cdb[3] = (block >> 16) & 0xFF;
    cdb[4] = (block >> 8) & 0xFF;
    cdb[5] = block & 0xFF;
    cdb[7] = (count >> 8) & 0xFF;
    cdb[8] = count & 0xFF;
    return msc_command(s, cdb, 10, buf, count * s->block_bytes,
                       write ? BOT_DIR_OUT : BOT_DIR_IN, NULL);
}

static int storage_read512(usb_storage_t *s, uint32_t lba, uint8_t *out) {
    uint32_t per, blk, off;

    if (s->block_bytes == SECTOR_SIZE)
        return msc_rw(s, lba, 1, out, 0);
    per = s->block_bytes / SECTOR_SIZE;
    if (per == 0)
        return -1;
    blk = lba / per;
    off = (lba % per) * SECTOR_SIZE;
    if (msc_rw(s, blk, 1, s->block_buf, 0) != 0)
        return -1;
    memcpy(out, s->block_buf + off, SECTOR_SIZE);
    return 0;
}

static int storage_write512(usb_storage_t *s, uint32_t lba, uint8_t *in) {
    uint32_t per, blk, off;

    if (s->block_bytes == SECTOR_SIZE)
        return msc_rw(s, lba, 1, in, 1);
    per = s->block_bytes / SECTOR_SIZE;
    if (per == 0)
        return -1;
    blk = lba / per;
    off = (lba % per) * SECTOR_SIZE;
    if (msc_rw(s, blk, 1, s->block_buf, 0) != 0)
        return -1;
    memcpy(s->block_buf + off, in, SECTOR_SIZE);
    return msc_rw(s, blk, 1, s->block_buf, 1);
}

static void usb_storage_read_sector(uint32_t lba, uint8_t *buf) {
    if (!g.present) {
        memset(buf, 0, SECTOR_SIZE);
        return;
    }
    mutex_lock(&g.lock);
    if (storage_read512(&g, lba, buf) != 0)
        memset(buf, 0, SECTOR_SIZE);
    mutex_unlock(&g.lock);
}

static void usb_storage_write_sector(uint32_t lba, uint8_t *buf) {
    if (!g.present)
        return;
    mutex_lock(&g.lock);
    storage_write512(&g, lba, buf);
    mutex_unlock(&g.lock);
}

static int usb_storage_probe(usb_device_t *dev) {
    uint8_t in = 0, out = 0;
    int have_in = 0, have_out = 0;
    uint32_t last_lba = 0, bs = 0;
    uint64_t total;
    int rc;

    if (!dev || !dev->hc || !dev->hc->bulk_transfer)
        return -1;
    if (g.present)
        return -1;

    for (int i = 0; i < dev->ep_count; i++) {
        usb_endpoint_t *ep = &dev->ep[i];
        if (ep->transfer_type != USB_TRANSFER_BULK)
            continue;
        if (ep->direction == USB_DIR_IN) { in = ep->address; have_in = 1; }
        else                            { out = ep->address; have_out = 1; }
    }
    if (!have_in || !have_out)
        return -1;

    memset(&g, 0, sizeof(g));
    g.dev         = dev;
    g.bulk_in     = in;
    g.bulk_out    = out;
    g.block_bytes = SECTOR_SIZE;
    mutex_init(&g.lock);

    msc_reset(&g);
    msc_get_max_lun(&g);

    /* A freshly powered drive needs a moment to spin up / answer. */
    rc = -1;
    for (int i = 0; i < 20; i++) {
        rc = msc_unit_ready(&g);
        if (rc == 0)
            break;
        ktime_busy_wait_us(100000);
    }
    if (rc != 0) {
        printk("\x01" "4" "usb_storage: device not ready\n");
        return -1;
    }

    if (msc_read_capacity(&g, &last_lba, &bs) != 0) {
        printk("\x01" "4" "usb_storage: READ CAPACITY failed\n");
        return -1;
    }
    if (bs == 0)
        bs = SECTOR_SIZE;
    g.block_bytes = bs;

    total = ((uint64_t)last_lba + 1) * bs / SECTOR_SIZE;
    if (total > 0xFFFFFFFFull)
        total = 0xFFFFFFFFull;
    g.max_lba = (uint32_t)total;

    if (bs != SECTOR_SIZE) {
        g.block_buf = (uint8_t *)kmalloc(bs);
        if (!g.block_buf) {
            printk("\x01" "4" "usb_storage: block buffer alloc failed\n");
            return -1;
        }
    }

    if (register_blkdev(STORAGE_DISK_NAME, g.max_lba,
                        usb_storage_read_sector,
                        usb_storage_write_sector) != 0) {
        printk("\x01" "4" "usb_storage: register_blkdev failed\n");
        if (g.block_buf)
            kfree(g.block_buf);
        g.block_buf = NULL;
        return -1;
    }

    g.present = 1;
    dev->driver_priv = &g;
    printk("\x01" "6" "usb_storage: " STORAGE_DISK_NAME
           " %u sectors, %u-byte blocks\n",
           (unsigned)g.max_lba, (unsigned)bs);
    return 0;
}

static void usb_storage_remove(usb_device_t *dev) {
    if (!g.present)
        return;
    unregister_blkdev(STORAGE_DISK_NAME);
    if (g.block_buf)
        kfree(g.block_buf);
    g.block_buf = NULL;
    g.dev       = NULL;
    g.present   = 0;
    dev->driver_priv = NULL;
}

static usb_driver_t usb_storage_driver = {
    .name       = "usb_storage",
    .class_code = USB_CLASS_MASS,   /* 0x08 */
    .subclass   = 0xFF,             /* 6 = SCSI transparent, but accept any */
    .protocol   = 0x50,             /* Bulk-Only Transport */
    .probe      = usb_storage_probe,
    .remove     = usb_storage_remove,
    .next       = NULL,
    .id_vendor  = 0,
    .id_product = 0,
};

int usb_driver_init(void) {
    return usb_driver_register(&usb_storage_driver);
}

void usb_driver_exit(void) {
    usb_driver_unregister(&usb_storage_driver);
    if (g.present) {
        unregister_blkdev(STORAGE_DISK_NAME);
        if (g.block_buf)
            kfree(g.block_buf);
        g.block_buf = NULL;
        g.present   = 0;
    }
}
