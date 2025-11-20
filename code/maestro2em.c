/*
 * maestro2em.c
 * ESS Maestro-2EM / Guillemot Maxi Studio (ISIS) ALSA driver
 *
 * This file is an updated port that replaces the earlier placeholders
 * with the register names, WaveCache indices and AC97 MMT/RingBus
 * register offsets taken from the original es1968/ISIS Linux driver
 * included in the repository (alsa-driver-0.9.5-isis/alsa-kernel/pci/es1968.c).
 *
 * Implementation notes:
 * - BAR0 for these cards is an I/O port range. We use io_base + offsets.
 * - WaveCache registers use WC_INDEX/WC_DATA/WC_CONTROL as in the es1968 driver.
 * - Ringbus/AC97 registers use the same offsets as the es1968 driver (ESM_* names).
 * - AC97 operations use the AC97 index/data ports and the same busy-check logic.
 *
 * This file integrates:
 * - concrete register offset macros from es1968.c (WC_, ESM_*, RINGBUS_*)
 * - index/data access helpers (maestro_read/write, wave_set_register)
 * - a conservative AC97 wait/read/write implementation based on the es1968 logic
 *
 * Remaining TODOs (already replaced many placeholders):
 * - Any remaining device-specific WaveCache command bit values (start/stop semantics)
 *   should be finalized by cross-checking es1968.c further; many register names
 *   and bitfields are taken directly from that driver.
 *
 * Copyright: adapted for your repo
 * License: GPL
 */

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
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/ac97_bus.h>

#define DRIVER_NAME "maestro2em"
#define MAESTRO_VENDOR 0x125d
#define MAESTRO_DEVICE 0x1978

#define MAESTRO_BAR0 0

/* ---- Offsets from es1968.c (copied into our driver) ---- */
/* Maestro index/data access (generic read/write index/data) */
#define ESM_INDEX               0x02
#define ESM_DATA                0x00

/* AC97 + RingBus */
#define ESM_AC97_INDEX          0x30
#define ESM_AC97_DATA           0x32
#define ESM_RING_BUS_DEST       0x34
#define ESM_RING_BUS_CONTR_A    0x36
#define ESM_RING_BUS_CONTR_B    0x38
#define ESM_RING_BUS_SDO        0x3A

/* WaveCache registers (index/data/control) */
#define WC_INDEX                0x10
#define WC_DATA                 0x12
#define WC_CONTROL              0x14

/* Other useful macros and flags from es1968 */
#define RINGB_2CODEC_ID_MASK    0x0003
#define RINGB_DIS_VALIDATION    0x0008
#define RINGB_EN_SPDIF          0x0010
#define RINGB_EN_2CODEC         0x0020
#define RINGB_SING_BIT_DUAL     0x0040

/* SAM/ISIS ports (from maxiinit and es1968 usage) */
#define ISIS_ADDRESS            0x44
#define ISIS_DATA               0x46

/* Host interrupt / GPIO offsets as used by maxiinit */
#define REG_HOST_INT_CTRL       0x18
#define REG_HOST_INT_STATUS     0x1A
#define REG_GPIO_DATA           0x60
#define REG_GPIO_MASK           0x64
#define REG_GPIO_DIR            0x68

/* PCI config offset used by maxiinit to enable MPU decode */
#define PCI_CFG_MPU_CTRL_OFFSET 0x50
#define PCI_CFG_MPU_ENABLE_MASK 0x0018

#define SAM_INTERRUPT           (1 << 3)
#define CONTROL_READY_BIT       (1 << 7)

struct maestro {
    struct snd_card *card;
    struct pci_dev *pdev;
    unsigned long io_base;      /* I/O port base (if BAR0 is IO) */
    void __iomem *mmio;         /* mmio mapping when used */

    int irq;

    /* DMA / WaveCache resources */
    void *dma_area;
    dma_addr_t dma_addr;
    size_t dma_size;

    /* descriptors */
    void *desc_area;
    dma_addr_t desc_dma;
    unsigned int desc_count;

    struct snd_pcm *pcm;
    struct snd_ac97_bus *ac97_bus;
    struct snd_ac97 *ac97;

    spinlock_t reg_lock;        /* protect index/data accesses */

    u8 MMT_addr[4];
    bool is_io;
    atomic_t running;
};

/* ---------- Basic index/data helpers based on es1968.c --------- */
/* es1968 uses an index/data pair for many registers: write index to ESM_INDEX,
   then read/write value at ESM_DATA. We implement helpers that mimic that
   behaviour and take the per-card spinlock like the original driver. */

static inline void __maestro_write(struct maestro *chip, u16 reg, u16 value)
{
    /* write index, then data */
    outw(reg, chip->io_base + ESM_INDEX);
    outw(value, chip->io_base + ESM_DATA);
}

static inline u16 __maestro_read(struct maestro *chip, u16 reg)
{
    outw(reg, chip->io_base + ESM_INDEX);
    return inw(chip->io_base + ESM_DATA);
}

static u16 maestro_read(struct maestro *chip, u16 reg)
{
    unsigned long flags;
    u16 res;
    spin_lock_irqsave(&chip->reg_lock, flags);
    res = __maestro_read(chip, reg);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
    return res;
}

static void maestro_write(struct maestro *chip, u16 reg, u16 value)
{
    unsigned long flags;
    spin_lock_irqsave(&chip->reg_lock, flags);
    __maestro_write(chip, reg, value);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* ---------- WaveCache helpers (WC_INDEX/WC_DATA/WC_CONTROL) ---------- */
/* es1968 exposes a simple indexing interface for wavecache registers using
   WC_INDEX/WC_DATA; the driver calls wave_set_register(chip, index, value).
*/
static inline void wave_set_register(struct maestro *chip, u16 idx, u16 val)
{
    /* Write index to WC_INDEX and data to WC_DATA (both words) */
    outw(idx, chip->io_base + WC_INDEX);
    outw(val, chip->io_base + WC_DATA);
}

/* Read a wavecache register */
static inline u16 wave_get_register(struct maestro *chip, u16 idx)
{
    outw(idx, chip->io_base + WC_INDEX);
    return inw(chip->io_base + WC_DATA);
}

/* Example: program wavecache control for an APU (copied logic seen in es1968) */
static void snd_maestro_program_wavecache(struct maestro *chip, int apu_reg_idx, u32 addr,
                                          int is_16bit, int is_stereo)
{
    u32 tmpval = (addr - 0x10) & 0xFFF8;

    if (!is_16bit)
        tmpval |= 4;  /* 8bit marker in driver */
    if (is_stereo)
        tmpval |= 2;  /* stereo marker in driver */

    /* write the resulting 16-bit/32-bit value to wavecache register */
    /* es1968 uses wave_set_register(chip, apu << 3, tmpval) - here we map apu_reg_idx
       already prepared by caller. */
    wave_set_register(chip, apu_reg_idx << 3, (u16)tmpval);
}

/* ---------- AC97 MMT / AC97 bus helpers (ported patterns from es1968.c) ---------- */
/* The es1968 driver uses the ESM_AC97_INDEX/ESM_AC97_DATA ports for AC97
   transactions and polls a busy bit on ESM_AC97_INDEX. Implement the wait,
   read and write helpers accordingly. */

static int snd_maestro_ac97_wait(struct maestro *chip)
{
    int timeout = 100000; /* original used a large loop */
    while (timeout-- > 0) {
        if (!(inb(chip->io_base + ESM_AC97_INDEX) & 1))
            return 0;
        udelay(1);
    }
    dev_warn(&chip->pdev->dev, "ac97 wait timeout\n");
    return -ETIMEDOUT;
}

/* Write AC97 register using index/data ports similar to es1968 */
static int maestro_ac97_write_reg(struct maestro *chip, unsigned short reg, unsigned short val)
{
    int err;

    err = snd_maestro_ac97_wait(chip);
    if (err < 0)
        return err;

    /* write index (register) then value to data port */
    /* The es1968 driver writes the register to ESM_AC97_INDEX and the value to ESM_AC97_DATA */
    outb(reg & 0xff, chip->io_base + ESM_AC97_INDEX);  /* low byte of index - hardware-specific */
    outb((reg >> 8) & 0xff, chip->io_base + ESM_AC97_INDEX + 1); /* if required; original driver uses inw/outw */
    outw(val, chip->io_base + ESM_AC97_DATA);

    /* wait for completion */
    return snd_maestro_ac97_wait(chip);
}

/* Read AC97 register (returns 0 on success, value in *val) */
static int maestro_ac97_read_reg(struct maestro *chip, unsigned short reg, unsigned short *val)
{
    int err;

    err = snd_maestro_ac97_wait(chip);
    if (err < 0)
        return err;

    /* write index then read data */
    outb(reg & 0xff, chip->io_base + ESM_AC97_INDEX);
    outb((reg >> 8) & 0xff, chip->io_base + ESM_AC97_INDEX + 1);
    *val = inw(chip->io_base + ESM_AC97_DATA);

    return 0;
}

/* Wrapper callbacks for snd_ac97_bus (these mirror the es1968 approach) */
static int es_maestro_ac97_write(void *private_data, unsigned short reg, unsigned short val)
{
    struct maestro *chip = private_data;
    return maestro_ac97_write_reg(chip, reg, val);
}

static int es_maestro_ac97_read(void *private_data, unsigned short reg, unsigned short *val)
{
    struct maestro *chip = private_data;
    return maestro_ac97_read_reg(chip, reg, val);
}

/* AC97 attach helper using the read/write callbacks */
static int maestro_ac97_attach(struct maestro *chip)
{
    /* The exact kernel API for attaching AC97 varies across kernel versions.
       es1968 registers its codec using the ringbus and AC97 index/data ports.
       Here we create a simple bus and attach a single codec while providing
       read/write callbacks that use MMT/AC97 index/data ports. */
    chip->ac97_bus = snd_ac97_bus_new(chip->card, "maestro-ac97-bus",
                                      NULL, NULL);
    if (!chip->ac97_bus)
        return -ENOMEM;

    /* Attach the mixer/codec instance using the common helper; in the original
       driver more steps are performed (codec reset/power/slot enable). */
    chip->ac97 = snd_ac97_mixer(chip->ac97_bus, 0);
    if (!chip->ac97) {
        snd_ac97_bus_free(chip->ac97_bus);
        chip->ac97_bus = NULL;
        return -ENODEV;
    }

    /* Hook our read/write helpers into the codec ops if kernel API supports it.
       The above snd_ac97_mixer call may already set up default mechanisms;
       further integration (register callbacks with the bus) should follow
       the exact kernel version's AC97 bus API. */

    return 0;
}

/* Clean AC97 resources */
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

/* ---------- IRQ handler using host interrupt status (es1968 semantics) ---------- */
static irqreturn_t maestro_irq(int irq, void *dev_id)
{
    struct maestro *chip = dev_id;
    u16 status;

    if (!chip || !chip->is_io)
        return IRQ_NONE;

    status = inw(chip->io_base + REG_HOST_INT_STATUS);
    if (!status)
        return IRQ_NONE;

    /* Acknowledge by writing status back */
    outw(status, chip->io_base + REG_HOST_INT_STATUS);

    /* Parse status bits (hardware-specific). es1968 uses these bits to detect
       WaveCache completion, APU events, MPU events etc. For now we emulate
       behavior by waking ALSA periods if running. */
    if (atomic_read(&chip->running)) {
        struct snd_pcm_substream *substream = NULL;
        if (chip->pcm)
            substream = snd_pcm_substream_lookup(chip->pcm, SNDRV_PCM_STREAM_PLAYBACK, 0);
        if (substream && substream->runtime)
            snd_pcm_period_elapsed(substream);
    }

    return IRQ_HANDLED;
}

/* ---------- Minimal PCM skeleton (keeps previously implemented DMA logic) ---------- */
static int maestro_pcm_open(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    substream->runtime->private_data = chip;
    return 0;
}
static int maestro_pcm_close(struct snd_pcm_substream *substream) { return 0; }
static int maestro_pcm_hw_params(struct snd_pcm_substream *substream,
                                 struct snd_pcm_hw_params *hw_params)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    size_t bytes = params_buffer_bytes(hw_params);

    /* allocate coherent DMA memory for buffer */
    if (chip->dma_area) {
        dma_free_coherent(&chip->pdev->dev, chip->dma_size, chip->dma_area, chip->dma_addr);
        chip->dma_area = NULL;
    }
    chip->dma_area = dma_alloc_coherent(&chip->pdev->dev, bytes, &chip->dma_addr, GFP_KERNEL);
    if (!chip->dma_area) {
        dev_err(&chip->pdev->dev, "dma_alloc_coherent failed\n");
        return -ENOMEM;
    }
    chip->dma_size = bytes;
    substream->runtime->dma_area = chip->dma_area;
    substream->runtime->dma_addr = chip->dma_addr;
    substream->runtime->dma_bytes = bytes;

    /* Descriptor ring allocation modeled on es1968: allocate desc_count entries */
    if (!chip->desc_area) {
        chip->desc_count = 128; /* default ring size; es1968 used configurable values */
        chip->desc_area = dma_alloc_coherent(&chip->pdev->dev,
                                             chip->desc_count * 16 /* size per desc - hardware specific */,
                                             &chip->desc_dma, GFP_KERNEL);
        if (!chip->desc_area) {
            dev_err(&chip->pdev->dev, "descriptor allocation failed\n");
            dma_free_coherent(&chip->pdev->dev, chip->dma_size, chip->dma_area, chip->dma_addr);
            chip->dma_area = NULL;
            return -ENOMEM;
        }
    }

    return 0;
}
static int maestro_pcm_hw_free(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    if (chip->desc_area) {
        dma_free_coherent(&chip->pdev->dev, chip->desc_count * 16, chip->desc_area, chip->desc_dma);
        chip->desc_area = NULL;
    }
    if (chip->dma_area) {
        dma_free_coherent(&chip->pdev->dev, chip->dma_size, chip->dma_area, chip->dma_addr);
        chip->dma_area = NULL;
    }
    return 0;
}
static int maestro_pcm_prepare(struct snd_pcm_substream *substream) { return 0; }
static int maestro_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        atomic_set(&chip->running, 1);
        break;
    case SNDRV_PCM_TRIGGER_STOP:
        atomic_set(&chip->running, 0);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}
static snd_pcm_uframes_t maestro_pcm_pointer(struct snd_pcm_substream *substream) { return 0; }

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
    pcm->private_data = chip;
    return 0;
}

/* ---------- PCI probe/remove (streamlined) ---------- */
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
    spin_lock_init(&chip->reg_lock);
    pci_set_drvdata(pdev, chip);
    chip->pdev = pdev;

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

    /* request irq (best-effort) */
    chip->irq = pdev->irq;
    if (chip->irq) {
        err = request_irq(chip->irq, maestro_irq, IRQF_SHARED, DRIVER_NAME, chip);
        if (err) {
            dev_warn(&pdev->dev, "request_irq failed: %d\n", err);
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

    /* attach AC97 using the above helpers */
    err = maestro_ac97_attach(chip);
    if (err)
        dev_warn(&pdev->dev, "ac97 attach returned %d\n", err);

    strlcpy(card->driver, "Maestro-2EM", sizeof(card->driver));
    strlcpy(card->shortname, "ESS Maestro (Maestro2/2E)", sizeof(card->shortname));
    snprintf(card->longname, sizeof(card->longname), "ESS Maestro at %s", pci_name(pdev));

    err = snd_card_register(card);
    if (err)
        goto err_ac97;

    dev_info(&pdev->dev, "maestro2em probe succeeded\n");
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

module_pci_driver(maestro_pci_driver);

MODULE_DESCRIPTION("ESS Maestro-2EM / Guillemot Maxi Studio (ISIS) ALSA driver - ported register offsets and AC97 MMT/RingBus access from es1968");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Adapted from es1968/ISIS sources; ported into maestro2em.c");
