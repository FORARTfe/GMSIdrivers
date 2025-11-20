// maestro2em.c
// Extended ALSA PCI driver for Guillemot Maxi Studio (ISIS / ESS Maestro).
// Adds WaveCache / DMA descriptor ring and AC97 MMT codec glue ported from maxiinit.

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/ac97_bus.h>

#define DRIVER_NAME "maestro2em"
#define MAESTRO_VENDOR 0x125d
#define MAESTRO_DEVICE 0x1978

/* PCI BAR and register offsets (IO ports) */
#define MAESTRO_BAR0 0

/* Important IO offsets (relative to IO base) discovered in maxiinit */
#define REG_HOST_INT_CTRL   0x18
#define REG_HOST_INT_STATUS 0x1A
#define REG_RINGBUS_A       0x36
#define REG_GPIO_DATA       0x60
#define REG_GPIO_MASK       0x64
#define REG_GPIO_DIR        0x68

/* ISIS SAM ports (relative to IO base) */
#define ISIS_ADDRESS 0x44
#define ISIS_DATA    0x46

/* PCI config offset used by maxiinit to enable MPU decode */
#define PCI_CFG_MPU_CTRL_OFFSET 0x50
#define PCI_CFG_MPU_ENABLE_MASK 0x0018

/* SAM/ISIS specifics */
#define SAM_INTERRUPT      (1 << 3)
#define CONTROL_READY_BIT  (1 << 7)

/* Default firmware name (use request_firmware) */
static char *firmware_name = "pci64.bin";
module_param(firmware_name, charp, 0444);
MODULE_PARM_DESC(firmware_name, "Firmware file name for MaxiStudio (skip offset 0x400)");

/* WaveCache/DMA settings (tunable via module params) */
static unsigned int wc_ring_size = 256; /* number of descriptors in ring (must be power of two) */
module_param(wc_ring_size, uint, 0444);
MODULE_PARM_DESC(wc_ring_size, "Number of WaveCache descriptors in ring (power of two)");

/* Per-card runtime structure */
struct maestro {
    struct snd_card *card;
    struct pci_dev *pdev;
    void __iomem *mmio;         /* not used for I/O-mode devices */
    unsigned long io_base;      /* I/O port base (if BAR0 is IO) */
    int irq;
    dma_addr_t dma_addr;        /* phys addr of coherent DMA buffer for PCM (per-stream allocated) */
    void *dma_area;             /* virt addr of coherent DMA buffer (per stream) */
    size_t dma_size;
    struct snd_pcm *pcm;
    struct snd_ac97_bus *ac97_bus;
    struct snd_ac97 *ac97;
    spinlock_t lock;
    u8 MMT_addr[4];
    bool is_io;                 /* true if BAR0 is IO ports */

    /* WaveCache descriptor ring */
    dma_addr_t desc_dma;        /* phys addr of descriptor ring */
    void *desc_area;            /* virt addr of descriptor ring */
    unsigned int desc_count;    /* number of descriptors */
    unsigned int cur_play;      /* software pointer: current playing descriptor index */
    unsigned int cur_submit;    /* software pointer: next descriptor to submit */
    atomic_t running;
};

/* ---------- IDS / placeholders for WaveCache registers ----------
   The real driver uses dedicated registers for descriptor base, head, tail, control, etc.
   Fill these constants from the original es1968/ISIS ALSA driver or datasheet.
*/
#define REG_WAVECACHE_DESC_BASE_LOW   0x100  /* TODO: replace with actual register offset */
#define REG_WAVECACHE_DESC_BASE_HIGH  0x104  /* TODO */
#define REG_WAVECACHE_DESC_COUNT      0x108  /* TODO */
#define REG_WAVECACHE_CMD             0x10C  /* TODO: start/stop command */
#define REG_WAVECACHE_STATUS          0x110  /* TODO: status/position register */
#define WAVECACHE_CMD_START           0x01   /* TODO: actual start bit */
#define WAVECACHE_CMD_STOP            0x02   /* TODO */

/* ---------- IRQ handler ---------- */
static irqreturn_t maestro_irq(int irq, void *dev_id)
{
    struct maestro *chip = dev_id;
    u16 status;

    if (!chip)
        return IRQ_NONE;

    if (!chip->is_io)
        return IRQ_NONE;

    /* Read host interrupt status (word) */
    status = inw(chip->io_base + REG_HOST_INT_STATUS);
    if (!status)
        return IRQ_NONE;

    /* Acknowledge handled bits (write back) */
    outw(status, chip->io_base + REG_HOST_INT_STATUS);

    /* NOTE: status parsing is hardware-specific. Common events:
          - WaveCache descriptor completion
          - WaveCache buffer completion (playback/capture)
          - MPU/SAM events
       The bits and handling must be filled in from the original driver.
    */
    /* Example: if bit X indicates WaveCache playback IRQ, then wake the ALSA buffer */
    /* For now, we use a generic wake path: if driver running, call snd_pcm_period_elapsed */
    if (atomic_read(&chip->running)) {
        struct snd_pcm_substream *substream;
        /* We don't track per-substream here; in a real driver track substreams and runtimes */
        /* This is a best-effort demonstration: iterate card's PCM and wake the first runtime */
        if (chip->pcm) {
            substream = snd_pcm_substream_lookup(chip->pcm, SNDRV_PCM_STREAM_PLAYBACK, 0);
            if (substream && substream->runtime)
                snd_pcm_period_elapsed(substream);
        }
    }

    return IRQ_HANDLED;
}

/* ---------- ISIS protocol helpers (port of maxiinit functions) ---------- */
#define ISIS_PRE_READ_DELAY_US 100
#define ISIS_PRE_WRITE_DELAY_US 100

static inline void isis_select_channel(struct maestro *chip, u8 sel)
{
    outb(sel, chip->io_base + ISIS_ADDRESS);
}

static void isis_write_control(struct maestro *chip, u8 data)
{
    isis_select_channel(chip, 0x1);
    udelay(ISIS_PRE_WRITE_DELAY_US);
    outb(data, chip->io_base + ISIS_DATA);
}

static u8 isis_read_control(struct maestro *chip)
{
    u8 result;
    isis_select_channel(chip, 0x1);
    udelay(ISIS_PRE_READ_DELAY_US);
    result = inb(chip->io_base + ISIS_DATA);
    return result;
}

static void isis_write_data8(struct maestro *chip, u8 data)
{
    isis_select_channel(chip, 0x0);
    udelay(ISIS_PRE_WRITE_DELAY_US);
    outb(data, chip->io_base + ISIS_DATA);
}

static u8 isis_read_data8(struct maestro *chip)
{
    u8 result;
    isis_select_channel(chip, 0x0);
    udelay(ISIS_PRE_READ_DELAY_US);
    result = inb(chip->io_base + ISIS_DATA);
    return result;
}

static void isis_write_data16(struct maestro *chip, u16 data)
{
    isis_select_channel(chip, 0x2);
    outw(data, chip->io_base + ISIS_DATA);
}

static void isis_burstwrite_data16(struct maestro *chip, const u16 *data, unsigned int len)
{
    unsigned int i;
    isis_select_channel(chip, 0x2);
    for (i = 0; i < len; i++)
        outw(data[i], chip->io_base + ISIS_DATA);
}

static int isis_wait_control_ready(struct maestro *chip, unsigned int timeout_ms)
{
    unsigned int elapsed = 0;
    while (elapsed < timeout_ms) {
        u8 ctrl = isis_read_control(chip);
        if (ctrl & CONTROL_READY_BIT)
            return 0;
        msleep(1);
        elapsed++;
    }
    return -ETIMEDOUT;
}

/* ---------- WaveCache descriptor format (generic) ----------
   Replace/adjust this struct to the exact format used by the hardware/driver.
   Keep it 64-bit aligned and consistent with the device endianness.
*/
struct wc_desc {
    u32 addr_low;    /* physical address low */
    u32 addr_high;   /* physical address high, if needed */
    u32 length;      /* buffer length in bytes or frames */
    u32 flags;       /* control flags: owner/interrupt/eof/loop etc. */
} __packed;

/* Helper: write descriptor ring registers (placeholder) */
static void write_wavcache_desc_regs(struct maestro *chip)
{
    u32 low = (u32)(chip->desc_dma & 0xffffffff);
    u32 high = (u32)(chip->desc_dma >> 32);
    /* TODO: replace with the device's correct descriptor base registers offsets */
    outl(low, chip->io_base + REG_WAVECACHE_DESC_BASE_LOW);
    outl(high, chip->io_base + REG_WAVECACHE_DESC_BASE_HIGH);
    outl(chip->desc_count, chip->io_base + REG_WAVECACHE_DESC_COUNT);
}

/* Start WaveCache engine (placeholder) */
static void wavcache_start(struct maestro *chip)
{
    /* TODO: write the correct start command to device register */
    outl(WAVECACHE_CMD_START, chip->io_base + REG_WAVECACHE_CMD);
    atomic_set(&chip->running, 1);
}

/* Stop WaveCache engine (placeholder) */
static void wavcache_stop(struct maestro *chip)
{
    /* TODO: write the correct stop command to device register */
    outl(WAVECACHE_CMD_STOP, chip->io_base + REG_WAVECACHE_CMD);
    atomic_set(&chip->running, 0);
}

/* ---------- PCM operations (WaveCache-backed) ---------- */

/* helper to compute bytes needed from hw_params */
static size_t hw_params_buffer_bytes(struct snd_pcm_hw_params *params)
{
    return params_buffer_bytes(params);
}

static int maestro_pcm_open(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    runtime->private_data = chip;
    return 0;
}

static int maestro_pcm_close(struct snd_pcm_substream *substream)
{
    /* nothing special */
    return 0;
}

static int maestro_pcm_hw_params(struct snd_pcm_substream *substream,
                                 struct snd_pcm_hw_params *hw_params)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    size_t buf_bytes = hw_params_buffer_bytes(hw_params);

    /* allocate physically contiguous coherent DMA buffer for the stream */
    if (chip->dma_area) {
        /* Already allocated; free then reallocate */
        dma_free_coherent(&chip->pdev->dev, chip->dma_size, chip->dma_area, chip->dma_addr);
        chip->dma_area = NULL;
    }

    chip->dma_size = buf_bytes;
    chip->dma_area = dma_alloc_coherent(&chip->pdev->dev, chip->dma_size, &chip->dma_addr, GFP_KERNEL);
    if (!chip->dma_area) {
        dev_err(&chip->pdev->dev, "dma_alloc_coherent failed (size=%zu)\n", chip->dma_size);
        return -ENOMEM;
    }

    runtime->dma_bytes = chip->dma_size;
    runtime->dma_area = chip->dma_area;
    runtime->dma_addr = chip->dma_addr;

    dev_info(&chip->pdev->dev, "allocated DMA buffer: virt=%p phys=%pad size=%zu\n",
             chip->dma_area, &chip->dma_addr, chip->dma_size);

    /* Allocate descriptor ring if not already present */
    if (!chip->desc_area) {
        chip->desc_count = wc_ring_size;
        chip->desc_area = dma_alloc_coherent(&chip->pdev->dev,
                                             chip->desc_count * sizeof(struct wc_desc),
                                             &chip->desc_dma, GFP_KERNEL);
        if (!chip->desc_area) {
            dev_err(&chip->pdev->dev, "descriptor ring alloc failed\n");
            dma_free_coherent(&chip->pdev->dev, chip->dma_size, chip->dma_area, chip->dma_addr);
            chip->dma_area = NULL;
            return -ENOMEM;
        }
        memset(chip->desc_area, 0, chip->desc_count * sizeof(struct wc_desc));
        write_wavcache_desc_regs(chip);
        dev_info(&chip->pdev->dev, "allocated descriptor ring virt=%p phys=%pad count=%u\n",
                 chip->desc_area, &chip->desc_dma, chip->desc_count);
    }

    /* Initialize ring pointers */
    chip->cur_play = 0;
    chip->cur_submit = 0;

    return 0;
}

static int maestro_pcm_hw_free(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);

    if (chip->desc_area) {
        dma_free_coherent(&chip->pdev->dev, chip->desc_count * sizeof(struct wc_desc),
                          chip->desc_area, chip->desc_dma);
        chip->desc_area = NULL;
        chip->desc_dma = 0;
    }

    if (chip->dma_area) {
        dma_free_coherent(&chip->pdev->dev, chip->dma_size, chip->dma_area, chip->dma_addr);
        chip->dma_area = NULL;
        chip->dma_addr = 0;
        chip->dma_size = 0;
    }

    return 0;
}

static int maestro_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);

    /* Program descriptor ring base/count into device registers */
    write_wavcache_desc_regs(chip);

    /* Pre-fill first descriptor(s) with the DMA buffer: for demonstration,
       we create a single descriptor that covers the whole user buffer. In
       a production driver you typically split into period-sized descriptors. */
    if (chip->desc_area) {
        struct wc_desc *d = (struct wc_desc *)chip->desc_area;
        d[0].addr_low = (u32)(chip->dma_addr & 0xffffffff);
        d[0].addr_high = (u32)(chip->dma_addr >> 32);
        d[0].length = chip->dma_size;
        d[0].flags = 0x1; /* owner bit; and set interrupt on completion if hardware uses it */
        /* Mark other descriptors as empty based on device descriptor semantics */
    }

    /* Ensure writes are visible to device */
    wmb();

    return 0;
}

static int maestro_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        wavcache_start(chip);
        break;
    case SNDRV_PCM_TRIGGER_STOP:
        wavcache_stop(chip);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static snd_pcm_uframes_t maestro_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);

    /* If hardware provides a current pointer register, read it and return bytes->frames().
       Placeholder: return software-maintained position derived from descriptor index. */
    /* TODO: read and convert hardware pointer */
    return 0;
}

/* PCM ops struct */
static const struct snd_pcm_ops maestro_pcm_ops = {
    .open = maestro_pcm_open,
    .close = maestro_pcm_close,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params = maestro_pcm_hw_params,
    .hw_free = maestro_pcm_hw_free,
    .prepare = maestro_pcm_prepare,
    .trigger = maestro_pcm_trigger,
    .pointer = maestro_pcm_pointer,
};

/* Create PCM instance */
static int maestro_create_pcm(struct maestro *chip)
{
    int err;
    struct snd_pcm *pcm;

    err = snd_pcm_new(chip->card, "Maestro PCM", 0, 1, 1, &pcm);
    if (err < 0)
        return err;
    chip->pcm = pcm;
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &maestro_pcm_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &maestro_pcm_ops);

    /* configure boundaries and memory ops as required */
    pcm->private_data = chip;
    return 0;
}

/* ---------- AC97 MMT-based codec glue ----------
   These callbacks implement a simple read/write using the MMT interface
   exposed by the SAM via ISIS CONTROL/DATA channels. They are a direct
   translation of maxiinit behavior; verify timings and sequences against
   the original es1968 ALSA driver and adjust delays/registers accordingly.
*/

/* Write one AC97 register (address/reg) using MMT path */
static int ac97_mmt_write(void *private_data, unsigned short reg, unsigned short val)
{
    struct maestro *chip = private_data;

    if (!chip || !chip->is_io)
        return -ENODEV;

    /* The real MMT protocol is device-specific. Here we implement a generic
       sequence:
         1) Select CONTROL and issue a MMT command to write AC97 register
         2) Send address and data bytes on DATA8
       Replace with exact sequence from es1968 driver if available.
    */
    if (isis_wait_control_ready(chip, 500) < 0)
        return -ETIMEDOUT;

    /* Example command sequence - highly device-specific - VERIFY with original driver */
    isis_write_control(chip, 0x40); /* placeholder MMT write command */
    isis_write_data8(chip, (u8)(reg & 0xff));
    isis_write_data8(chip, (u8)((reg >> 8) & 0xff));
    isis_write_data8(chip, (u8)(val & 0xff));
    isis_write_data8(chip, (u8)((val >> 8) & 0xff));

    /* Flush / wait for completion */
    if (isis_wait_control_ready(chip, 500) < 0)
        return -ETIMEDOUT;

    return 0;
}

/* Read one AC97 register into *val using MMT path */
static int ac97_mmt_read(void *private_data, unsigned short reg, unsigned short *val)
{
    struct maestro *chip = private_data;
    u8 b0, b1;

    if (!chip || !chip->is_io)
        return -ENODEV;

    if (isis_wait_control_ready(chip, 500) < 0)
        return -ETIMEDOUT;

    /* Example MMT read command - device-specific and must be replaced with exact sequence */
    isis_write_control(chip, 0x41); /* placeholder MMT read command */
    isis_write_data8(chip, (u8)(reg & 0xff));
    isis_write_data8(chip, (u8)((reg >> 8) & 0xff));

    if (isis_wait_control_ready(chip, 500) < 0)
        return -ETIMEDOUT;

    b0 = isis_read_data8(chip);
    b1 = isis_read_data8(chip);
    *val = (u16)b0 | (((u16)b1) << 8);

    return 0;
}

/* AC97 bus attach using MMT read/write callbacks */
static int maestro_ac97_attach(struct maestro *chip)
{
    int err = 0;
    struct snd_ac97_bus_ops ops = {
        .write = ac97_mmt_write,
        .read = ac97_mmt_read,
    };

    /* Create AC97 bus. Note: kernel API versions vary. Adapt as needed. */
    chip->ac97_bus = snd_ac97_bus_new(chip->card, "maestro-ac97-bus",
                                      NULL /* read_fn */, NULL /* write_fn */);
    if (!chip->ac97_bus)
        return -ENOMEM;

    /* Attach codec with simple ops wrapper using private_data = chip */
    chip->ac97 = snd_ac97_mixer(chip->ac97_bus, 0);
    if (!chip->ac97) {
        snd_ac97_bus_free(chip->ac97_bus);
        chip->ac97_bus = NULL;
        return -ENODEV;
    }

    /* Note: Many kernels require registering bus ops differently. This code is
       a placeholder showing intent: replace with the proper API calls. */
    /* TODO: Replace snd_ac97_mixer usage with proper ops registration that calls ac97_mmt_read/write */

    return err;
}

static void maestro_cleanup_ac97(struct maestro *chip)
{
    if (!chip)
        return;
    if (chip->ac97) {
        snd_ac97_del(chip->ac97);
        chip->ac97 = NULL;
    }
    if (chip->ac97_bus) {
        snd_ac97_bus_free(chip->ac97_bus);
        chip->ac97_bus = NULL;
    }
}

/* ---------- samBoot and firmware upload (as in previous step) ---------- */
/* samBoot array omitted here for brevity - use the same samBoot[] from maxiinit or previous file.
   In a real driver keep the samBoot array and firmware upload routine previously implemented.
*/

/* ---------- PCI probe and remove (streamlined) ---------- */

static const struct pci_device_id maestro_ids[] = {
    { PCI_DEVICE(MAESTRO_VENDOR, MAESTRO_DEVICE) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, maestro_ids);

static int maestro_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct maestro *chip;
    struct snd_card *card;
    int err;
    resource_size_t bar0_start, bar0_len;

    err = pci_enable_device(pdev);
    if (err)
        return err;

    pci_set_master(pdev);

    chip = kzalloc(sizeof(*chip), GFP_KERNEL);
    if (!chip) {
        err = -ENOMEM;
        goto err_disable;
    }
    spin_lock_init(&chip->lock);
    chip->pdev = pdev;
    pci_set_drvdata(pdev, chip);

    bar0_start = pci_resource_start(pdev, MAESTRO_BAR0);
    bar0_len = pci_resource_len(pdev, MAESTRO_BAR0);

    if (pci_resource_flags(pdev, MAESTRO_BAR0) & IORESOURCE_IO) {
        unsigned long io_start = (unsigned long)(bar0_start & 0xFFFEUL);
        if (!request_region(io_start, bar0_len ? bar0_len : 1, DRIVER_NAME)) {
            err = -EBUSY;
            goto err_free;
        }
        chip->io_base = io_start;
        chip->is_io = true;
    } else {
        chip->mmio = pci_iomap(pdev, MAESTRO_BAR0, 0);
        if (!chip->mmio) {
            err = -EIO;
            goto err_free;
        }
        chip->is_io = false;
    }

    /* try to upload firmware if IO-mode and needed; previously implemented routine should be used */
    /* maestro_upload_firmware(chip); -- assumed done earlier in probe flow */

    /* Request IRQ (best-effort) */
    chip->irq = pdev->irq;
    if (chip->irq) {
        err = request_irq(chip->irq, maestro_irq, IRQF_SHARED, DRIVER_NAME, chip);
        if (err) {
            dev_warn(&pdev->dev, "request_irq failed: %d (continuing)\n", err);
            /* continue */
        }
    }

    err = snd_card_new(&pdev->dev, -1, "maestro", THIS_MODULE, 0, &card);
    if (err)
        goto err_irq;
    chip->card = card;
    card->private_data = chip;

    err = maestro_create_pcm(chip);
    if (err)
        goto err_card;

    err = maestro_ac97_attach(chip);
    if (err)
        dev_warn(&pdev->dev, "ac97 attach returned %d (may still work)\n", err);

    strlcpy(card->driver, "Maestro-2EM", sizeof(card->driver));
    strlcpy(card->shortname, "Guillemot Maxi Studio (ISIS / Maestro)", sizeof(card->shortname));
    snprintf(card->longname, sizeof(card->longname), "Guillemot Maxi Studio at %s", pci_name(pdev));

    err = snd_card_register(card);
    if (err)
        goto err_ac97;

    dev_info(&pdev->dev, "maestro driver probed\n");
    return 0;

err_ac97:
    maestro_cleanup_ac97(chip);
err_card:
    snd_card_free(card);
err_irq:
    if (chip->irq)
        free_irq(chip->irq, chip);
    if (chip->is_io)
        release_region(chip->io_base, bar0_len ? bar0_len : 1);
    else if (chip->mmio)
        pci_iounmap(pdev, chip->mmio);
err_free:
    kfree(chip);
err_disable:
    pci_disable_device(pdev);
    return err;
}

static void maestro_remove(struct pci_dev *pdev)
{
    struct maestro *chip = pci_get_drvdata(pdev);
    if (!chip)
        return;

    maestro_cleanup_ac97(chip);

    if (chip->card)
        snd_card_free(chip->card);

    if (chip->irq)
        free_irq(chip->irq, chip);

    if (chip->desc_area)
        dma_free_coherent(&chip->pdev->dev, chip->desc_count * sizeof(struct wc_desc),
                          chip->desc_area, chip->desc_dma);

    if (chip->dma_area)
        dma_free_coherent(&chip->pdev->dev, chip->dma_size, chip->dma_area, chip->dma_addr);

    if (chip->is_io)
        release_region(chip->io_base, pci_resource_len(pdev, MAESTRO_BAR0) ? pci_resource_len(pdev, MAESTRO_BAR0) : 1);
    else if (chip->mmio)
        pci_iounmap(pdev, chip->mmio);

    kfree(chip);
    pci_disable_device(pdev);
}

static struct pci_driver maestro_pci_driver = {
    .name = DRIVER_NAME,
    .id_table = maestro_ids,
    .probe = maestro_probe,
    .remove = maestro_remove,
};

static int __init maestro_init(void)
{
    return pci_register_driver(&maestro_pci_driver);
}
static void __exit maestro_exit(void)
{
    pci_unregister_driver(&maestro_pci_driver);
}

module_init(maestro_init);
module_exit(maestro_exit);

MODULE_DESCRIPTION("Guillemot Maxi Studio (ISIS / ESS Maestro) ALSA driver - WaveCache/DMA and AC97 glue (incomplete)");
MODULE_AUTHOR("Adapted from maxiinit and maestro2em skeleton");
MODULE_LICENSE("GPL");
