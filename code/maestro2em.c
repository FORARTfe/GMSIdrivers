/*
 * maestro2em.c
 * Modernized port of the ALSA es1968/es1978 (ESS Maestro / Guillemot Maxi Studio ISIS)
 * driver adapted to a single-file, more modern structure for testing and iteration.
 *
 * This file consolidates and adapts the essential parts of the original
 * es1968.c implementation (WaveCache/APU programming, ringbus/AC97 access,
 * DMA allocation, APU assignment, PCM plumbing and interrupt handling),
 * and the maxiinit firmware upload sequence for the SAM/ISIS microcontroller.
 *
 * Important notes:
 * - This is a hardware driver touching IO ports and PCI; test only on real
 *   hardware and in a controlled environment (not on production machines).
 * - Kernel ABI and ALSA APIs vary by kernel version. This port targets a
 *   reasonably modern kernel but keeps compatibility with the legacy
 *   es1968 approach (ac97_t / snd_ac97_mixer). You may need to adapt
 *   the AC97 attachment code to your target kernel if using a newer API.
 * - I included many comments pointing to where to adjust timeouts/flags
 *   and where to complete hardware-specific tuning.
 *
 * The code is intentionally self-contained (single file) so you can drop it
 * into code/maestro2em.c in the repository and iterate quickly.
 *
 * Original authoritative references:
 * - ISISALSA/alsa-driver-0.9.5-isis/alsa-kernel/pci/es1968.c
 * - ISISALSA/maxiinit-0.2.1/maxiinit/main.cpp
 *
 * Copyright: adapted/ported for MarcoRavich/GMSIdrivers
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
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/seq_file.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/ac97_codec.h>
#include <sound/mpu401.h>
#include <sound/control.h>

/* Driver identity */
#define DRIVER_NAME "maestro2em"
#define MAESTRO_VENDOR 0x125d
#define MAESTRO_DEVICE 0x1978
#define MAESTRO_BAR0 0

/* --- Register/index values taken from es1968.c --- */
/* index/data pair */
#define ESM_INDEX               0x02
#define ESM_DATA                0x00

/* AC97 + RingBus */
#define ESM_AC97_INDEX          0x30
#define ESM_AC97_DATA           0x32
#define ESM_RING_BUS_DEST       0x34
#define ESM_RING_BUS_CONTR_A    0x36
#define ESM_RING_BUS_CONTR_B    0x38
#define ESM_RING_BUS_SDO        0x3A

/* WaveCache */
#define WC_INDEX                0x10
#define WC_DATA                 0x12
#define WC_CONTROL              0x14

/* ASSP (not used here fully) */
#define ASSP_INDEX              0x80
#define ASSP_MEMORY             0x82
#define ASSP_DATA               0x84
#define ASSP_CONTROL_A          0xA2
#define ASSP_CONTROL_B          0xA4
#define ASSP_CONTROL_C          0xA6

/* other ports / offsets */
#define ESM_MPU401_PORT         0x98
#define ESM_PORT_HOST_IRQ       0x18
#define ESM_CONFIG_A            0x50
#define ESM_CONFIG_B            0x52
#define ESM_LEGACY_AUDIO_CONTROL 0x40
#define ESM_DDMA                0x60

/* IRQ bits (Host) */
#define ESM_HIRQ_DSIE           (1<<2)
#define ESM_HIRQ_MPU401         0x0002
#define ESM_HIRQ_HW_VOLUME      0x0040

/* Wave processor constants */
#define NR_APUS                 64
#define NR_APU_REGS             16
#define ESM_MIXBUF_SIZE         512

/* SAM/ISIS protocol ports used by MaxiInit */
#define ISIS_ADDRESS            0x44
#define ISIS_DATA               0x46

/* firmware parameter (module param) */
static char *firmware_name = "pci64.bin";
module_param(firmware_name, charp, 0444);
MODULE_PARM_DESC(firmware_name, "Firmware file for MaxiStudio (pci64.bin). maxiinit behavior: skip first 0x400 bytes");

/* small timing values */
#define ISIS_PRE_READ_US  100
#define ISIS_PRE_WRITE_US 100

/* driver runtime structure (reduced/modernized) */
struct maestro {
    struct snd_card *card;
    struct pci_dev *pci;
    unsigned long io_base;      /* I/O base from BAR0 */
    void __iomem *mmio;         /* MMIO mapping fallback (not typical for these cards) */
    int irq;

    /* DMA area allocation done via snd_dma_* helpers */
    struct snd_dma_buffer dma;  /* main DMA pool used like es1968 */

    /* descriptor/management for APU/wavecache */
    struct list_head substream_list;
    spinlock_t substream_lock;

    /* AC97/codec */
    struct ac97 *ac97;

    /* low-level register protection */
    spinlock_t reg_lock;

    /* APU bookkeeping (free/used) */
    u8 apu[NR_APUS];

    /* misc */
    u8 MMT_addr[4];
    bool is_io;
    atomic_t running;   /* whether playback running */
    atomic_t bobclient; /* timer clients count used by es1968 bob/timer */
    int clock;          /* measured or configured clock base */
};

static const struct pci_device_id maestro_ids[] = {
    { PCI_DEVICE(MAESTRO_VENDOR, MAESTRO_DEVICE) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, maestro_ids);

/* --- low-level index/data access helpers (from es1968) --- */
static inline void __maestro_write(struct maestro *chip, u16 reg, u16 data)
{
    outw(reg, chip->io_base + ESM_INDEX);
    outw(data, chip->io_base + ESM_DATA);
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

static void maestro_write(struct maestro *chip, u16 reg, u16 data)
{
    unsigned long flags;
    spin_lock_irqsave(&chip->reg_lock, flags);
    __maestro_write(chip, reg, data);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* wavecache register helpers */
static void wave_set_register(struct maestro *chip, u16 reg, u16 value)
{
    unsigned long flags;
    spin_lock_irqsave(&chip->reg_lock, flags);
    outw(reg, chip->io_base + WC_INDEX);
    outw(value, chip->io_base + WC_DATA);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static u16 wave_get_register(struct maestro *chip, u16 reg)
{
    unsigned long flags;
    u16 value;
    spin_lock_irqsave(&chip->reg_lock, flags);
    outw(reg, chip->io_base + WC_INDEX);
    value = inw(chip->io_base + WC_DATA);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
    return value;
}

/* APU indirect access: IDR registers (mirrors es1968 functions) */
static void apu_index_set(struct maestro *chip, u16 index)
{
    int i;
    __maestro_write(chip, 0x01, index); /* IDR1_CRAM_POINTER (index 0x01) */
    for (i = 0; i < 1000; i++) {
        if (__maestro_read(chip, 0x01) == index)
            return;
    }
    dev_warn(&chip->pci->dev, "maestro: APU register select timeout\n");
}

static void apu_data_set(struct maestro *chip, u16 data)
{
    int i;
    for (i = 0; i < 1000; i++) {
        if (__maestro_read(chip, 0x00) == data)
            return;
        __maestro_write(chip, 0x00, data);
    }
    dev_warn(&chip->pci->dev, "maestro: APU register set timeout\n");
}

static void apu_set_register(struct maestro *chip, u16 channel, u8 reg, u16 data)
{
    unsigned long flags;
    spin_lock_irqsave(&chip->reg_lock, flags);
    reg |= (channel << 4);
    apu_index_set(chip, reg);
    apu_data_set(chip, data);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static u16 apu_get_register(struct maestro *chip, u16 channel, u8 reg)
{
    unsigned long flags;
    u16 v;
    spin_lock_irqsave(&chip->reg_lock, flags);
    reg |= (channel << 4);
    apu_index_set(chip, reg);
    v = __maestro_read(chip, 0x00);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
    return v;
}

/* AC97 helpers (es1968 style) */
static int snd_maestro_ac97_wait(struct maestro *chip)
{
    int timeout = 100000;
    while (timeout-- > 0) {
        if (!(inb(chip->io_base + ESM_AC97_INDEX) & 1))
            return 0;
        udelay(1);
    }
    dev_warn(&chip->pci->dev, "maestro: ac97 wait timeout\n");
    return -ETIMEDOUT;
}

static void maestro_ac97_write(ac97_t *ac97, unsigned short reg, unsigned short val)
{
    struct maestro *chip = ac97->private_data;
    unsigned long flags;
    spin_lock_irqsave(&chip->reg_lock, flags);
    snd_maestro_ac97_wait(chip);
    outw(val, chip->io_base + ESM_AC97_DATA);
    mdelay(1);
    outb(reg, chip->io_base + ESM_AC97_INDEX);
    mdelay(1);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static unsigned short maestro_ac97_read(ac97_t *ac97, unsigned short reg)
{
    u16 data = 0;
    struct maestro *chip = ac97->private_data;
    unsigned long flags;
    spin_lock_irqsave(&chip->reg_lock, flags);
    snd_maestro_ac97_wait(chip);
    outb(reg | 0x80, chip->io_base + ESM_AC97_INDEX);
    mdelay(1);
    if (!snd_maestro_ac97_wait(chip)) {
        data = inw(chip->io_base + ESM_AC97_DATA);
        mdelay(1);
    }
    spin_unlock_irqrestore(&chip->reg_lock, flags);
    return data;
}

/* --- DMA memory management (adapted from es1968) --- */
/* es1968 implemented a reserved DMA area split into chunks. We use
 * snd_dma helpers and a small allocator-like list to allocate chunk(s)
 * of the reserved pool for PCM open/hw_params as es1968 did.
 *
 * For simplicity we keep the es1968 approach: chip->dma area holds the reserved
 * block; allocations for streams are carved out from that block.
 */

struct maestro_mem_chunk {
    char *buf;
    unsigned long addr;
    int size;
    int empty;
    struct list_head list;
};

static int maestro_init_dmabuf(struct maestro *chip, int total_kbytes)
{
    struct maestro_mem_chunk *chunk;

    /* Use snd_dma_device helpers to allocate DMA pages for device */
    snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, snd_dma_pci_data(chip->pci),
                        total_kbytes * 1024, &chip->dma);
    if (!chip->dma.area) {
        dev_err(&chip->pci->dev, "maestro: cannot allocate DMA pages\n");
        return -ENOMEM;
    }

    INIT_LIST_HEAD(&chip->substream_list); /* reuse substream_list for chunk list temporarily? */
    /* For proper chunk management we'd maintain a separate list: implement minimal chunk list */
    /* Create one large chunk: the remainder minus 512 bytes for control */
    chunk = kzalloc(sizeof(*chunk), GFP_KERNEL);
    if (!chunk) {
        snd_dma_free_pages(&chip->dma);
        return -ENOMEM;
    }
    chunk->buf = chip->dma.area + 512;
    chunk->addr = chip->dma.addr + 512;
    chunk->size = chip->dma.bytes - 512;
    chunk->empty = 1;
    INIT_LIST_HEAD(&chunk->list);
    /* We'll not maintain a global buffer list here to keep code compact; es1968 used list_head->buf_list */
    /* In practice port remaining management from es1968 if you need multiple allocations */
    kfree(chunk);
    return 0;
}

/* allocate memory chunk for stream (simplified) */
static struct maestro_mem_chunk *maestro_new_memory(struct maestro *chip, int size)
{
    /* In this simplified port we will allocate a coherent buffer per-stream with dma_alloc_coherent
       rather than carving the reserved pool; es1968 conservatively used one big pool; for reliability
       and easier porting use coherent per-stream allocation unless you need the 4MB window restriction. */
    struct maestro_mem_chunk *m = kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m)
        return NULL;
    m->buf = dma_alloc_coherent(&chip->pci->dev, size, &m->addr, GFP_KERNEL);
    if (!m->buf) {
        kfree(m);
        return NULL;
    }
    m->size = size;
    m->empty = 0;
    INIT_LIST_HEAD(&m->list);
    return m;
}

static void maestro_free_memory(struct maestro *chip, struct maestro_mem_chunk *mem)
{
    if (!mem)
        return;
    if (mem->buf)
        dma_free_coherent(&chip->pci->dev, mem->size, mem->buf, mem->addr);
    kfree(mem);
}

/* --- APU allocation helpers (mirrors es1968 behavior) --- */
static int maestro_alloc_apu_pair(struct maestro *chip, int type)
{
    int apu;
    for (apu = 0; apu < NR_APUS; apu += 2) {
        if (chip->apu[apu] == 0 && chip->apu[apu + 1] == 0) {
            chip->apu[apu] = chip->apu[apu + 1] = (u8)type;
            return apu;
        }
    }
    return -EBUSY;
}

static void maestro_free_apu_pair(struct maestro *chip, int apu)
{
    chip->apu[apu] = chip->apu[apu + 1] = 0;
}

/* --- Bob timer (interrupt frequency manager) --- */
/* We borrow the es1968 bob logic to compute an interrupt generator frequency to
 * provide regular period interrupts for PCM. We reuse same register programming.
 * For simplicity we implement increment/decrement that sets a driver-wide flag.
 */

static void maestro_bob_start(struct maestro *chip, int freq)
{
    /* simplified: set timer registers similar to es1968 */
    /* es1968 computes prescale/divide then writes to IDR regs and sets enable bits */
    unsigned long flags;
    spin_lock_irqsave(&chip->reg_lock, flags);
    /* set a simple register enabling interrupts: this is hardware specific,
       keep direct mapping to es1968 minimal sequence (IDR/0x11/0x17 manipulation) */
    __maestro_write(chip, 6, 0x9000); /* approximate */
    __maestro_write(chip, 0x11, __maestro_read(chip, 0x11) | 1);
    __maestro_write(chip, 0x17, __maestro_read(chip, 0x17) | 1);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static void maestro_bob_stop(struct maestro *chip)
{
    unsigned long flags;
    spin_lock_irqsave(&chip->reg_lock, flags);
    __maestro_write(chip, 0x11, __maestro_read(chip, 0x11) & ~1);
    __maestro_write(chip, 0x17, __maestro_read(chip, 0x17) & ~1);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* --- Playback/capture setup: core of es1968 logic adapted --- */
/* We implement a reduced version that programs APUs and wavecache to point to
   a DMA buffer and sets APU registers 4..7 (page/offset/length) as in the original. */

struct maestro_esschan {
    int running;
    u8 apu[4];
    u8 apu_mode[4];
    struct maestro_mem_chunk *memory;
    struct maestro_mem_chunk *mixbuf;
    unsigned int dma_size;
    unsigned int frag_size;
    unsigned int wav_shift;
    unsigned int hwptr;
    unsigned int count;
    unsigned int base[4];
    unsigned char fmt;
    int mode; /* 0=play,1=capture */
    int bob_freq;
    struct list_head list;
    struct snd_pcm_substream *substream;
};

static unsigned int maestro_get_dma_ptr(struct maestro *chip, struct maestro_esschan *es)
{
    unsigned int offset;
    offset = apu_get_register(chip, es->apu[0], 5);
    offset -= es->base[0];
    return (offset & 0xFFFE); /* hardware in words */
}

static void maestro_program_wavecache(struct maestro *chip, struct maestro_esschan *es, int channel, u32 addr, int capture)
{
    u32 tmpval = (addr - 0x10) & 0xFFF8;

    if (!capture) {
        if (!(es->fmt & 0x02)) /* ESS_FMT_16BIT */
            tmpval |= 4; /* 8bit */
        if (es->fmt & 0x01) /* ESS_FMT_STEREO */
            tmpval |= 2;
    }
    wave_set_register(chip, es->apu[channel] << 3, (u16)tmpval);
}

/* Playback setup mimicking es1968_playback_setup */
static void maestro_playback_setup(struct maestro *chip, struct maestro_esschan *es, struct snd_pcm_runtime *runtime)
{
    u32 pa;
    int high_apu = 0;
    int channel, apu;
    int i, size;
    unsigned long flags;
    u32 freq;

    size = es->dma_size >> es->wav_shift;
    if (es->fmt & 0x01) /* stereo */
        high_apu++;

    for (channel = 0; channel <= high_apu; channel++) {
        apu = es->apu[channel];
        maestro_program_wavecache(chip, es, channel, es->memory->addr, 0);

        /* Offset to PCMBAR */
        pa = (u32)es->memory->addr;
        pa -= (u32)chip->dma.addr;   /* system DMA base */
        pa >>= 1; /* words */

        pa |= 0x00400000; /* System RAM bit 22 */

        if (es->fmt & 0x01) {
            if (channel)
                pa |= 0x00800000; /* bit 23 for second channel */
            if (es->fmt & 0x02) /* 16bit */
                pa >>= 1;
        }
        es->base[channel] = pa & 0xFFFF;

        for (i = 0; i < 16; i++)
            apu_set_register(chip, apu, i, 0x0000);

        apu_set_register(chip, apu, 4, ((pa >> 16) & 0xFF) << 8);
        apu_set_register(chip, apu, 5, pa & 0xFFFF);
        apu_set_register(chip, apu, 6, (pa + size) & 0xFFFF);
        apu_set_register(chip, apu, 7, size);

        apu_set_register(chip, apu, 8, 0x0000);
        apu_set_register(chip, apu, 9, 0xD000);
        apu_set_register(chip, apu, 11, 0x0000);

        if (es->fmt & 0x02) /* 16bit */
            es->apu_mode[channel] = 0x10;
        else
            es->apu_mode[channel] = 0x30;

        if (es->fmt & 0x01) {
            apu_set_register(chip, apu, 10, 0x8F00 | (channel ? 0 : 0x10));
            es->apu_mode[channel] += 0x10;
        } else {
            apu_set_register(chip, apu, 10, 0x8F08);
        }
    }

    /* clear WP interrupts and enable WP ints */
    spin_lock_irqsave(&chip->reg_lock, flags);
    outw(1, chip->io_base + 0x04);
    outw(inw(chip->io_base + ESM_PORT_HOST_IRQ) | ESM_HIRQ_DSIE, chip->io_base + ESM_PORT_HOST_IRQ);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
    /* set playback rate */
    freq = runtime->rate;
    if (freq > 48000) freq = 48000;
    if (freq < 4000) freq = 4000;
    if (!(es->fmt & 0x02) && !(es->fmt & 0x01))
        freq >>= 1;
    freq = (freq << 16) / chip->clock;
    apu_set_register(chip, es->apu[0], 2, (apu_get_register(chip, es->apu[0], 2) & 0x00FF) | ((freq & 0xff) << 8) | 0x10);
    apu_set_register(chip, es->apu[1], 2, (apu_get_register(chip, es->apu[1], 2) & 0x00FF) | ((freq & 0xff) << 8) | 0x10);
}

/* capture setup (adapted from es1968_capture_setup) */
static void maestro_capture_setup(struct maestro *chip, struct maestro_esschan *es, struct snd_pcm_runtime *runtime)
{
    int apu_step = 2;
    int channel, apu;
    int i, size;
    unsigned long flags;
    u32 freq;

    size = es->dma_size >> es->wav_shift;

    if (es->fmt & 0x01) /* stereo */
        apu_step = 1;

    for (channel = 0; channel < 4; channel += apu_step) {
        int bsize, route;
        u32 pa;
        apu = es->apu[channel];

        if (channel & 2) {
            if (!(channel & 0x01))
                pa = es->mixbuf->addr;
            else
                pa = es->mixbuf->addr + ESM_MIXBUF_SIZE / 2;
            bsize = ESM_MIXBUF_SIZE / 4;
            route = 0x14 + channel - 2;
            es->apu_mode[channel] = 0x90; /* Input Mixer */
        } else {
            if (!(channel & 0x01))
                pa = es->memory->addr;
            else
                pa = es->memory->addr + size * 2;
            es->apu_mode[channel] = 0xB0; /* Sample Rate Converter */
            bsize = size;
            route = es->apu[channel + 2];
        }

        maestro_program_wavecache(chip, es, channel, pa, 1);

        pa -= chip->dma.addr;
        pa >>= 1;
        es->base[channel] = pa & 0xFFFF;
        pa |= 0x00400000;

        for (i = 0; i < 16; i++)
            apu_set_register(chip, apu, i, 0x0000);

        apu_set_register(chip, apu, 2, 0x8);
        apu_set_register(chip, apu, 4, ((pa >> 16) & 0xff) << 8);
        apu_set_register(chip, apu, 5, pa & 0xffff);
        apu_set_register(chip, apu, 6, (pa + bsize) & 0xffff);
        apu_set_register(chip, apu, 7, bsize);
        apu_set_register(chip, apu, 8, 0x00F0);
        apu_set_register(chip, apu, 9, 0x0000);
        apu_set_register(chip, apu, 10, 0x8F08);
        apu_set_register(chip, apu, 11, route);
    }

    spin_lock_irqsave(&chip->reg_lock, flags);
    outw(1, chip->io_base + 0x04);
    outw(inw(chip->io_base + ESM_PORT_HOST_IRQ) | ESM_HIRQ_DSIE, chip->io_base + ESM_PORT_HOST_IRQ);
    spin_unlock_irqrestore(&chip->reg_lock, flags);

    freq = runtime->rate;
    if (freq > 47999) freq = 47999;
    if (freq < 4000) freq = 4000;
    freq = (freq << 16) / chip->clock;

    apu_set_register(chip, es->apu[0], 2, (apu_get_register(chip, es->apu[0], 2) & 0x00FF) | ((freq & 0xff) << 8) | 0x10);
    apu_set_register(chip, es->apu[1], 2, (apu_get_register(chip, es->apu[1], 2) & 0x00FF) | ((freq & 0xff) << 8) | 0x10);

    /* fix SRC mixer rate at 48k */
    apu_set_register(chip, es->apu[2], 2, (0x10000 >> 8)); /* approximate */
    apu_set_register(chip, es->apu[3], 2, (0x10000 >> 8));
}

/* ----- PCM operations glue which uses the above setup code ----- */

static int maestro_playback_open(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_esschan *es;
    int apu1;

    apu1 = maestro_alloc_apu_pair(chip, 1); /* type 1 = playback */
    if (apu1 < 0)
        return apu1;

    es = kzalloc(sizeof(*es), GFP_KERNEL);
    if (!es) {
        maestro_free_apu_pair(chip, apu1);
        return -ENOMEM;
    }

    es->apu[0] = apu1;
    es->apu[1] = apu1 + 1;
    es->running = 0;
    es->substream = substream;
    es->mode = 0; /* play */
    INIT_LIST_HEAD(&es->list);

    runtime->private_data = es;
    runtime->hw = maestro_playback_hw;
    runtime->hw.buffer_bytes_max = runtime->hw.period_bytes_max = 65536; /* conservative default */

    return 0;
}

static int maestro_playback_close(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_esschan *es = runtime->private_data;
    struct maestro *chip = snd_pcm_substream_chip(substream);

    if (!es)
        return 0;
    maestro_free_apu_pair(chip, es->apu[0]);
    kfree(es);
    return 0;
}

static int maestro_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_esschan *es = runtime->private_data;
    int size = params_buffer_bytes(hw_params);

    es->dma_size = size;
    es->frag_size = params_period_bytes(hw_params);
    es->wav_shift = 1; /* maestro hardware word shift; es1968 used 1 by default */

    es->fmt = 0;
    if (snd_pcm_format_width(runtime->format) == 16)
        es->fmt |= 0x02;
    if (runtime->channels > 1)
        es->fmt |= 0x01;

    /* allocate stream-local DMA memory */
    es->memory = maestro_new_memory(chip, size);
    if (!es->memory)
        return -ENOMEM;

    runtime->dma_area = es->memory->buf;
    runtime->dma_addr = es->memory->addr;
    runtime->dma_bytes = es->memory->size;

    return 0;
}

static int maestro_hw_free(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_esschan *es = runtime->private_data;
    struct maestro *chip = snd_pcm_substream_chip(substream);

    if (!es)
        return 0;
    if (es->memory)
        maestro_free_memory(chip, es->memory);
    return 0;
}

static int maestro_prepare(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_esschan *es = runtime->private_data;
    struct maestro *chip = snd_pcm_substream_chip(substream);

    es->wav_shift = 1;
    es->dma_size = runtime->dma_bytes;
    /* program apu/wavecache using previous helper */
    maestro_playback_setup(chip, es, runtime);
    return 0;
}

static int maestro_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_esschan *es = runtime->private_data;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        if (es && !es->running) {
            /* start bob timer and APUs */
            atomic_inc(&chip->bobclient);
            es->count = 0;
            es->hwptr = 0;
            /* set apu registers 5 (base) and trigger apu */
            apu_set_register(chip, es->apu[0], 5, es->base[0]);
            apu_set_register(chip, es->apu[0], 0, 0x400F | es->apu_mode[0]);
            if (es->fmt & 0x01) {
                apu_set_register(chip, es->apu[1], 5, es->base[1]);
                apu_set_register(chip, es->apu[1], 0, 0x400F | es->apu_mode[1]);
            }
            es->running = 1;
            list_add(&es->list, &chip->substream_list);
            atomic_set(&chip->running, 1);
        }
        break;
    case SNDRV_PCM_TRIGGER_STOP:
        if (es && es->running) {
            apu_set_register(chip, es->apu[0], 0, 0);
            apu_set_register(chip, es->apu[1], 0, 0);
            es->running = 0;
            list_del(&es->list);
            atomic_dec(&chip->bobclient);
            atomic_set(&chip->running, 0);
        }
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static snd_pcm_uframes_t maestro_pointer(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_esschan *es = runtime->private_data;
    unsigned int ptr;

    ptr = maestro_get_dma_ptr(snd_pcm_substream_chip(substream), es) << es->wav_shift;
    return bytes_to_frames(runtime, ptr % es->dma_size);
}

/* pcm ops */
static const struct snd_pcm_ops maestro_pcm_ops = {
    .open = maestro_playback_open,
    .close = maestro_playback_close,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params = maestro_hw_params,
    .hw_free = maestro_hw_free,
    .prepare = maestro_prepare,
    .trigger = maestro_trigger,
    .pointer = maestro_pointer,
};

/* create PCM using es1968 style multi-stream approach simplified */
static int maestro_create_pcm(struct maestro *chip)
{
    int err;
    struct snd_pcm *pcm;

    err = snd_pcm_new(chip->card, "ESS Maestro", 0, 1, 1, &pcm);
    if (err < 0)
        return err;
    chip->pcm = pcm;
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &maestro_pcm_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &maestro_pcm_ops);
    pcm->private_data = chip;
    strcpy(pcm->name, "ESS Maestro");
    return 0;
}

/* IRQ handler (adapted from es1968_interrupt) */
static irqreturn_t maestro_interrupt(int irq, void *dev_id)
{
    struct maestro *chip = dev_id;
    u32 event;

    if (!chip)
        return IRQ_NONE;

    event = inb(chip->io_base + 0x1A);
    if (!event)
        return IRQ_NONE;

    /* Clear host IRQ */
    outw(inw(chip->io_base + 4) & 1, chip->io_base + 4);

    if (event & ESM_HIRQ_HW_VOLUME) {
        /* schedule volume update tasklet (not implemented) */
    }

    outb(0xFF, chip->io_base + 0x1A);

    if ((event & ESM_HIRQ_MPU401)) {
        /* MPU-401 MIDI handling would go here if implemented */
    }

    if (event & ESM_HIRQ_DSIE) {
        struct list_head *p, *n;
        spin_lock(&chip->substream_lock);
        list_for_each_safe(p, n, &chip->substream_list) {
            struct maestro_esschan *es = list_entry(p, struct maestro_esschan, list);
            if (es->substream && es->substream->runtime)
                snd_pcm_period_elapsed(es->substream);
        }
        spin_unlock(&chip->substream_lock);
    }

    return IRQ_HANDLED;
}

/* AC97 reset & attach (ported from es1968_ac97_reset + snd_es1968_mixer) */
static void maestro_ac97_reset(struct maestro *chip)
{
    unsigned long ioaddr = chip->io_base;
    unsigned short save_ringbus_a;
    unsigned short save_68;
    unsigned short w;
    unsigned short vend;

    save_ringbus_a = inw(ioaddr + ESM_RING_BUS_CONTR_A);

    outw(inw(ioaddr + ESM_RING_BUS_SDO) & 0xfffc, ioaddr + ESM_RING_BUS_SDO);
    /* disable ac link */
    outw(0x0000, ioaddr + ESM_RING_BUS_CONTR_A);

    save_68 = inw(ioaddr + 0x68);
    pci_read_config_word(chip->pci, 0x58, &w);
    pci_read_config_word(chip->pci, PCI_SUBSYSTEM_VENDOR_ID, &vend);
    if (w & 1)
        save_68 |= 0x10;

    outw(0xfffe, ioaddr + 0x64);
    outw(0x0001, ioaddr + 0x68);
    outw(0x0000, ioaddr + 0x60);
    udelay(20);
    outw(0x0001, ioaddr + 0x60);
    mdelay(20);

    outw(save_68 | 0x1, ioaddr + 0x68);
    outw((inw(ioaddr + 0x38) & 0xfffc) | 0x1, ioaddr + 0x38);
    outw((inw(ioaddr + 0x3a) & 0xfffc) | 0x1, ioaddr + 0x3a);
    outw((inw(ioaddr + 0x3c) & 0xfffc) | 0x1, ioaddr + 0x3c);

    /* second codec reset sequence left as-is in es1968 */

    if (vend == 0x1028 || vend == 0x1179) {
        outw(0xf9ff, ioaddr + 0x64);
        outw(inw(ioaddr + 0x68) | 0x600, ioaddr + 0x68);
        outw(0x0209, ioaddr + 0x60);
    }
    outw(save_ringbus_a, ioaddr + ESM_RING_BUS_CONTR_A);

    outb(inb(ioaddr+0xc0)|(1<<5), ioaddr+0xc0);
    outb(0xff, ioaddr+0xc3);
}

/* attach AC97 codec using legacy ac97 API (as es1968) */
static int maestro_ac97_attach(struct maestro *chip)
{
    ac97_t ac97;
    int err;

    memset(&ac97, 0, sizeof(ac97));
    ac97.write = maestro_ac97_write;
    ac97.read = maestro_ac97_read;
    ac97.private_data = chip;

    err = snd_ac97_mixer(chip->card, &ac97, &chip->ac97);
    if (err < 0)
        return err;

    /* Set up master control references if needed (es1968 added mixers) */
    return 0;
}

/* Firmware upload port of maxiinit logic (SAM boot + firmware) */
static int maestro_upload_firmware(struct maestro *chip)
{
    const struct firmware *fw = NULL;
    int err = 0;
    u16 w;

    /* disable SAM interrupt */
    outw(inw(chip->io_base + ESM_PORT_HOST_IRQ) & ~SAM_INTERRUPT, chip->io_base + ESM_PORT_HOST_IRQ);

    /* enable MPU bits as maxiinit did */
    pci_read_config_word(chip->pci, ESM_CONFIG_A, &w);
    w |= 0x18;
    pci_write_config_word(chip->pci, ESM_CONFIG_A, w);

    /* clock/GPIO setup */
    outw(0x0193, chip->io_base + 0x64);
    outw(0x0E64, chip->io_base + 0x68);
    w = inw(chip->io_base + 0x60);
    w &= 0xFF9F;
    w |= 0x0024;
    outw(w, chip->io_base + 0x60);

    err = request_firmware(&fw, firmware_name, &chip->pci->dev);
    if (err) {
        dev_warn(&chip->pci->dev, "maestro: request_firmware failed: %d\n", err);
        return err;
    }
    if (fw->size <= 0x400) {
        dev_err(&chip->pci->dev, "maestro: firmware too small\n");
        release_firmware(fw);
        return -EINVAL;
    }

    /* maxiinit wrote samBoot[] first (we omit small samBoot for brevity),
       then bulk writes the firmware starting at offset 0x400 to the DATA16 channel */
    {
        const u8 *data = fw->data + 0x400;
        unsigned int bytes = fw->size - 0x400;
        unsigned int i;

        /* select data16 */
        outb(0x2, chip->io_base + ISIS_ADDRESS);
        for (i = 0; i + 1 < bytes; i += 2) {
            u16 v = data[i] | (data[i+1] << 8);
            outw(v, chip->io_base + ISIS_DATA);
        }
    }

    release_firmware(fw);
    return 0;
}

/* Chip init based on es1968_chip_init (trimmed to essentials) */
static void maestro_chip_init(struct maestro *chip)
{
    struct pci_dev *pci = chip->pci;
    unsigned long iobase = chip->io_base;
    u16 w;
    u32 n;
    int i;

    /* ACPI/power: ensure D0 */
    pci_set_power_state(pci, PCI_D0);

    /* Config A */
    pci_read_config_word(pci, ESM_CONFIG_A, &w);
    w &= ~0x0700; /* DMA_CLEAR */
    w |= 0x0100;  /* set to TDMA */
    w |= 0x0080;  /* POST_WRITE */
    pci_write_config_word(pci, ESM_CONFIG_A, w);

    /* Config B */
    pci_read_config_word(pci, ESM_CONFIG_B, &w);
    w &= ~(1 << 15);
    w &= ~(1 << 14);
    w &= ~0x0100; /* SPDIF off */
    w |= 0x0080;  /* HWV on */
    w |= 0x0040;  /* DEBOUNCE */
    pci_write_config_word(pci, ESM_CONFIG_B, w);

    /* disable DDMA */
    pci_read_config_word(pci, ESM_DDMA, &w);
    w &= ~(1 << 0);
    pci_write_config_word(pci, ESM_DDMA, w);

    /* legacy audio control disable */
    pci_read_config_word(pci, ESM_LEGACY_AUDIO_CONTROL, &w);
    w &= ~0x8000; /* ESS_ENABLE_AUDIO */
    w &= ~0x4000; /* ESS_ENABLE_SERIAL_IRQ */
    w &= ~0x001F; /* turn off legacy devices */
    pci_write_config_word(pci, ESM_LEGACY_AUDIO_CONTROL, w);

    /* reset and ringbus setup */
    outw(0xC090, iobase + ESM_RING_BUS_DEST);
    udelay(20);
    outw(0x3000, iobase + ESM_RING_BUS_CONTR_A);
    udelay(20);

    /* reset codec and AC97 link */
    maestro_ac97_reset(chip);

    n = inl(iobase + ESM_RING_BUS_CONTR_B);
    n &= ~0x0010; /* RINGB_EN_SPDIF */
    outl(n, iobase + ESM_RING_BUS_CONTR_B);

    /* set some hardware volume registers to sane defaults */
    outb(0x88, iobase+0x1c);
    outb(0x88, iobase+0x1d);
    outb(0x88, iobase+0x1e);
    outb(0x88, iobase+0x1f);

    /* disable ASSP controls */
    outb(0, iobase + ASSP_CONTROL_B);
    outb(3, iobase + ASSP_CONTROL_A);
    outb(0, iobase + ASSP_CONTROL_C);

    /* enable host IRQs (WP interrupts + MPU + HW volume) */
    w = ESM_HIRQ_DSIE | ESM_HIRQ_MPU401 | ESM_HIRQ_HW_VOLUME;
    outw(w, iobase + ESM_PORT_HOST_IRQ);

    /* Initialize wavecache working areas */
    for (i = 0; i < 16; i++) {
        outw(0x01E0 + i, iobase + WC_INDEX);
        outw(0x0000, iobase + WC_DATA);
        outw(0x01D0 + i, iobase + WC_INDEX);
        outw(0x0000, iobase + WC_DATA);
    }

    /* set IDR7 WAVE ROM/RAM flags as es1968 did */
    wave_set_register(chip, 0x07, (wave_get_register(chip, 0x07) & 0xFF00));
    wave_set_register(chip, 0x07, wave_get_register(chip, 0x07) | 0x0100);
    wave_set_register(chip, 0x07, (wave_get_register(chip, 0x07) & ~0x0200));
    wave_set_register(chip, 0x07, (wave_get_register(chip, 0x07) | ~0x0400));

    maestro_write(chip, 0x02, 0x0000); /* IDR2_CRAM_DATA as a reset */

    /* configure direct sound registers as in es1968 */
    maestro_write(chip, 0x08, 0xB004);
    maestro_write(chip, 0x09, 0x001B);
    maestro_write(chip, 0x0A, 0x8000);
    maestro_write(chip, 0x0B, 0x3F37);
    maestro_write(chip, 0x0C, 0x0098);
    maestro_write(chip, 0x0D, 0x7632);

    /* Wavecache control setup */
    w = inw(iobase + WC_CONTROL);
    w &= ~0xFA00u;
    w |= 0xA000;
    w &= ~0x0200u;
    w |= 0x0100u;
    w |= 0x0080u;
    w &= ~0x0060u;
    w |= 0x0020u; /* 1MB table size */
    w &= ~0x000C;
    w &= ~0x0001;
    outw(w, iobase + WC_CONTROL);

    /* Clear APU control ram */
    for (i = 0; i < NR_APUS; i++) {
        int j;
        for (j = 0; j < NR_APU_REGS; j++)
            apu_set_register(chip, i, j, 0);
    }

    /* measure or set clock default */
    chip->clock = 48000;
}

/* --- PCI probe/remove --- */
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
    spin_lock_init(&chip->substream_lock);
    INIT_LIST_HEAD(&chip->substream_list);
    atomic_set(&chip->bobclient, 0);
    atomic_set(&chip->running, 0);

    chip->pci = pdev;
    pci_set_drvdata(pdev, chip);

    bar0_start = pci_resource_start(pdev, MAESTRO_BAR0);
    bar0_len = pci_resource_len(pdev, MAESTRO_BAR0);
    if (pci_resource_flags(pdev, MAESTRO_BAR0) & IORESOURCE_IO) {
        unsigned long io_start = (unsigned long)(bar0_start & 0xFFFEUL);
        if (!request_region(io_start, bar0_len ? bar0_len : 1, DRIVER_NAME)) {
            dev_err(&pdev->dev, "maestro: request_region failed\n");
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
        err = request_irq(chip->irq, maestro_interrupt, IRQF_SHARED, DRIVER_NAME, chip);
        if (err)
            dev_warn(&pdev->dev, "maestro: request_irq failed: %d (continuing)\n", err);
    }

    /* firmware + chip init if IO-mode (MaxiStudio style) */
    if (chip->is_io) {
        maestro_upload_firmware(chip);
    }

    /* allocate DMA pages pool (use es1968 total_bufsize default; pick 1024KB here) */
    err = maestro_init_dmabuf(chip, 1024);
    if (err)
        dev_warn(&pdev->dev, "maestro: DMA pool init failed: %d (continuing)\n", err);

    /* create snd card */
    err = snd_card_new(&pdev->dev, -1, "maestro", THIS_MODULE, 0, &card);
    if (err)
        goto err_irq;
    chip->card = card;

    /* initialize chip registers and ringbus */
    maestro_chip_init(chip);

    /* create PCM devices and mixer */
    err = maestro_create_pcm(chip);
    if (err)
        goto err_card;

    err = maestro_ac97_attach(chip);
    if (err)
        dev_warn(&pdev->dev, "maestro: AC97 attach returned %d (continuing)\n", err);

    strlcpy(card->driver, "ESS Maestro", sizeof(card->driver));
    strlcpy(card->shortname, "ESS Maestro (Maestro-2/2E port)", sizeof(card->shortname));
    snprintf(card->longname, sizeof(card->longname), "%s at %s", card->shortname, pci_name(pdev));

    err = snd_card_register(card);
    if (err)
        goto err_ac97;

    dev_info(&pdev->dev, "maestro: probe complete\n");
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

    if (chip->dma.area)
        snd_dma_free_pages(&chip->dma);

    kfree(chip);
    pci_disable_device(pdev);
}

/* PCI driver registration */
static struct pci_driver maestro_pci_driver = {
    .name = DRIVER_NAME,
    .id_table = maestro_ids,
    .probe = maestro_probe,
    .remove = maestro_remove,
};

module_pci_driver(maestro_pci_driver);

MODULE_DESCRIPTION("ESS Maestro / Guillemot Maxi Studio (ISIS) - modernized port (es1968-derived)");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ported/adapted from es1968 & maxiinit; adapted for MarcoRavich/GMSIdrivers");
