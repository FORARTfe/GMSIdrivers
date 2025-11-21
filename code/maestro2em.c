/*
 * maestro2em.c
 * Modernized port of the ALSA es1968/es1978 (ESS Maestro / Guillemot Maxi Studio ISIS)
 * driver adapted to a single-file structure for testing and iteration.
 *
 * This revision implements the exact es1968 DMA-pool behavior:
 *  - reserved DMA pool allocation via snd_dma_device_pci / snd_dma_* helpers
 *  - chunk list allocation and splitting/coalescing (same semantics as es1968)
 *  - APIs: maestro_init_dmabuf, maestro_new_memory, maestro_free_memory,
 *    maestro_free_dmabuf (names adapted from original)
 *
 * The file also contains the ported index/data, WC/APU and AC97 helpers that
 * were previously added.  This focuses on reproducing the original driver's
 * DMA pool behavior so the WaveCache addressing and 28-bit window restrictions
 * match the hardware expectations.
 *
 * References:
 * - ISISALSA/alsa-driver-0.9.5-isis/alsa-kernel/pci/es1968.c (authoritative source)
 * - ISISALSA/maxiinit-0.2.1/maxiinit/main.cpp (firmware/init sequences)
 *
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
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/ac97_codec.h>
#include <sound/mpu401.h>
#include <sound/control.h>
#include <sound/dma.h> /* for snd_dma_device etc. */

#define DRIVER_NAME "maestro2em"
#define MAESTRO_VENDOR 0x125d
#define MAESTRO_DEVICE 0x1978
#define MAESTRO_BAR0 0

/* index/data pair */
#define ESM_INDEX               0x02
#define ESM_DATA                0x00

/* AC97 + RingBus */
#define ESM_AC97_INDEX          0x30
#define ESM_AC97_DATA           0x32

/* ASSP (APU) control */
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

/* Host IRQ bits */
#define ESM_HIRQ_DSIE           (1<<2)
#define ESM_HIRQ_MPU401         0x0002
#define SAM_INTERRUPT           0x0004 /* SAM/ISIS firmware interrupt bit (approximate) */
#define ESM_HIRQ_HW_VOLUME      0x0040
#define ESM_HIRQ_TIMER          0x0008

/* APU count (from es1968 driver) */
#define NR_APUS                 64

/* ISIS / SAM control ports (relative to chip->io_base) */
#define ISIS_DATA               0x46
#define ISIS_ADDRESS            0x44

#define ISIS_PRE_READ_US        100
#define ISIS_PRE_WRITE_US       100

/* GPIO ports (from maxiinit) */
#define ESM_GPIO_DATA           0x60
#define ESM_GPIO_MASK           0x64
#define ESM_GPIO_DIR            0x68

/* SAM interrupt bit (from maxiinit) */
#undef SAM_INTERRUPT
#define SAM_INTERRUPT           (1 << 3)

/* Stream format flags */
#define ESS_FMT_STEREO      0x01
#define ESS_FMT_16BIT       0x02

/* Stream mode */
#define MAESTRO_MODE_PLAY   0
#define MAESTRO_MODE_CAPTURE 1

/* Bob timer parameters (copied from es1968) */
#define ESS_SYSCLK          50000000
#define MAESTRO_BOB_FREQ    200
#define MAESTRO_BOB_FREQ_MAX 800

/* Host IRQ status bits (read at io_base + 0x1A) */
#define ESM_SOUND_IRQ       0x04   /* DirectSound / WaveCache IRQ */
#define ESM_HWVOL_IRQ       0x40

/* SAM / Dream interrupt bit in Host IRQ control */
#ifndef SAM_INTERRUPT
#define SAM_INTERRUPT       0x0008
#endif

/* Driver runtime structures (with es1968-style DMA pool) */
struct maestro_mem_chunk {
    char *buf;              /* virtual pointer to chunk */
    unsigned long addr;     /* bus address */
    int size;               /* size in bytes */
    int empty;              /* 1 == free */
    struct list_head list;
};

struct maestro {
    struct snd_card *card;
    struct pci_dev *pci;
    unsigned long io_base;
    void __iomem *mmio;
    int irq;

    /* DMA management (es1968-style reserved pool) */
    struct snd_dma_device dma_dev;
    struct snd_dma_buffer dma;   /* reserved DMA pool */
    struct list_head buf_list;   /* list of maestro_mem_chunk */

    int total_bufsize_kb;        /* configured total pool size (kB) */

    /* descriptors & APUs */
    void *desc_area;
    dma_addr_t desc_dma;
    unsigned int desc_count;

    struct snd_pcm *pcm;
    struct ac97 *ac97;

    spinlock_t reg_lock;
    spinlock_t substream_lock;

    u8 apu[NR_APUS];             /* APU usage map */
    u8 MMT_addr[4];
    bool is_io;
    atomic_t running;
    atomic_t bobclient;
    int clock;

    int bob_freq;                /* current Bob timer frequency */
    struct list_head substream_list; /* active PCM channels */
};

struct maestro_pcm_channel {
    int running;

    u8 apu[4];            /* APUs assigned to this stream */
    u8 apu_mode[4];       /* mode programmed in each APU */

    struct maestro_mem_chunk *memory; /* playback/capture buffer in DMA pool */
    struct maestro_mem_chunk *mixbuf; /* capture mix buffer (if needed) */

    unsigned int hwptr;   /* current hw pointer in bytes */
    unsigned int count;   /* bytes accumulated since last period */
    unsigned int dma_size;/* total buffer size in bytes */
    unsigned int frag_size; /* period size in bytes */
    unsigned int wav_shift;
    u16 base[4];          /* APU base offsets */

    unsigned char fmt;    /* ESS_FMT_* flags */
    int mode;             /* MAESTRO_MODE_PLAY / MAESTRO_MODE_CAPTURE */

    int bob_freq;         /* required Bob timer frequency for this stream */

    struct snd_pcm_substream *substream;

    struct list_head list; /* link in chip->substream_list */
};

static int maestro_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_pcm_channel *chan = runtime->private_data;

    if (!chan || !chan->memory)
        return -EINVAL;

    chan->hwptr = 0;
    chan->count = 0;

    if (chan->mode == MAESTRO_MODE_PLAY)
        maestro_playback_setup(chip, chan, runtime);
    /* capture_setup can be added here when you port it */

    return 0;
}

static int maestro_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct maestro_pcm_channel *chan = substream->runtime->private_data;

    spin_lock(&chip->substream_lock);

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
    case SNDRV_PCM_TRIGGER_RESUME:
        if (!chan->running) {
            maestro_bob_inc(chip, chan->bob_freq);
            chan->count = 0;
            chan->hwptr = 0;
            maestro_pcm_start(chip, chan);
            chan->running = 1;
        }
        break;

    case SNDRV_PCM_TRIGGER_STOP:
    case SNDRV_PCM_TRIGGER_SUSPEND:
        if (chan->running) {
            maestro_pcm_stop(chip, chan);
            chan->running = 0;
            maestro_bob_dec(chip);
        }
        break;
    }

    spin_unlock(&chip->substream_lock);
    return 0;
}

static snd_pcm_uframes_t maestro_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct maestro_pcm_channel *chan = substream->runtime->private_data;
    unsigned int ptr;

    ptr = maestro_get_dma_ptr(chip, chan) << chan->wav_shift;

    return bytes_to_frames(substream->runtime, ptr % chan->dma_size);
}

static snd_pcm_uframes_t maestro_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct maestro_pcm_channel *chan = substream->runtime->private_data;
    unsigned int ptr;

    ptr = maestro_get_dma_ptr(chip, chan) << chan->wav_shift;

    return bytes_to_frames(substream->runtime, ptr % chan->dma_size);
}


static const struct snd_pcm_hardware maestro_pcm_hw_playback = {
    .info = SNDRV_PCM_INFO_MMAP |
            SNDRV_PCM_INFO_INTERLEAVED |
            SNDRV_PCM_INFO_BLOCK_TRANSFER,
    .formats = SNDRV_PCM_FMTBIT_S16_LE,
    .rates = SNDRV_PCM_RATE_8000_48000,
    .rate_min = 8000,
    .rate_max = 48000,
    .channels_min = 1,
    .channels_max = 8,
    .buffer_bytes_max = 512 * 1024,   /* must not exceed DMA pool size */
    .period_bytes_min = 64,
    .period_bytes_max = 128 * 1024,
    .periods_min = 2,
    .periods_max = 1024,
    .fifo_size = 0,
};

static const struct snd_pcm_hardware maestro_pcm_hw_capture = {
    .info = SNDRV_PCM_INFO_MMAP |
            SNDRV_PCM_INFO_INTERLEAVED |
            SNDRV_PCM_INFO_BLOCK_TRANSFER,
    .formats = SNDRV_PCM_FMTBIT_S16_LE,
    .rates = SNDRV_PCM_RATE_8000_48000,
    .rate_min = 8000,
    .rate_max = 48000,
    .channels_min = 1,
    .channels_max = 8,
    .buffer_bytes_max = 512 * 1024,   /* must not exceed DMA pool size */
    .period_bytes_min = 64,
    .period_bytes_max = 128 * 1024,
    .periods_min = 2,
    .periods_max = 1024,
    .fifo_size = 0,
};

/* Open callback shared by playback and capture */

static int maestro_pcm_open_generic(struct snd_pcm_substream *substream,
                                    const struct snd_pcm_hardware *hw)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_pcm_channel *chan;

    chan = kzalloc(sizeof(*chan), GFP_KERNEL);
    if (!chan)
        return -ENOMEM;

    chan->chip = chip;
    chan->substream = substream;
    chan->mem = NULL;
    chan->buffer_bytes = 0;
    chan->period_bytes = 0;
    chan->hw_ptr = 0;
    chan->running = 0;
    spin_lock_init(&chan->lock);

    INIT_LIST_HEAD(&chan->list);

    runtime->private_data = chan;
    runtime->hw = *hw; /* copy capabilities */

    /* Add channel to chip's substream list for use in IRQ handler */
    spin_lock(&chip->substream_lock);
    list_add_tail(&chan->list, &chip->substream_list);
    spin_unlock(&chip->substream_lock);

    return 0;
}

static int maestro_pcm_open_playback(struct snd_pcm_substream *substream)
{
    return maestro_pcm_open_generic(substream, &maestro_pcm_hw_playback);
}

static int maestro_pcm_open_capture(struct snd_pcm_substream *substream)
{
    return maestro_pcm_open_generic(substream, &maestro_pcm_hw_capture);
}

static int maestro_pcm_close(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct maestro_pcm_channel *chan = substream->runtime->private_data;

    if (!chan)
        return 0;

    /* Remove from chip list */
    spin_lock(&chip->substream_lock);
    list_del(&chan->list);
    spin_unlock(&chip->substream_lock);

    /* If hw_free was not called for some reason, free pool memory here */
    if (chan->mem) {
        maestro_free_memory(chip, chan->mem);
        chan->mem = NULL;
    }

    kfree(chan);
    substream->runtime->private_data = NULL;

    return 0;
}

/* hw_params: allocate from maestro DMA pool */

static int maestro_pcm_hw_params(struct snd_pcm_substream *substream,
                                 struct snd_pcm_hw_params *params)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_pcm_channel *chan = runtime->private_data;
    struct maestro_mem_chunk *mem;
    size_t size;

    if (!chan)
        return -EINVAL;

    if (chan->mem) {
        /* already allocated, free first */
        maestro_free_memory(chip, chan->mem);
        chan->mem = NULL;
    }

    size = params_buffer_bytes(params);

    /* Allocate from internal DMA pool (es1968-style) */
    mem = maestro_new_memory(chip, size);
    if (!mem)
        return -ENOMEM;

    chan->mem = mem;
    chan->buffer_bytes = size;
    chan->period_bytes = params_period_bytes(params);
    chan->hw_ptr = 0;

    runtime->dma_area = mem->buf;
    runtime->dma_addr = mem->addr;
    runtime->dma_bytes = size;

    return 0;
}

/* hw_free: release memory back to DMA pool */

static int maestro_pcm_hw_free(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_pcm_channel *chan = runtime->private_data;

    if (!chan)
        return 0;

    if (chan->mem) {
        maestro_free_memory(chip, chan->mem);
        chan->mem = NULL;
    }

    runtime->dma_area = NULL;
    runtime->dma_addr = 0;
    runtime->dma_bytes = 0;

    chan->buffer_bytes = 0;
    chan->period_bytes = 0;
    chan->hw_ptr = 0;

    return 0;
}

static void maestro_update_pcm(struct maestro *chip,
                               struct maestro_pcm_channel *chan)
{
    unsigned int hwptr;
    unsigned int diff;
    struct snd_pcm_substream *subs = chan->substream;

    if (!subs || !chan->running)
        return;

    hwptr = maestro_get_dma_ptr(chip, chan) << chan->wav_shift;
    hwptr %= chan->dma_size;

    diff = (chan->dma_size + hwptr - chan->hwptr) % chan->dma_size;

    chan->hwptr = hwptr;
    chan->count += diff;

    if (chan->count >= chan->frag_size) {
        spin_unlock(&chip->substream_lock);
        snd_pcm_period_elapsed(subs);
        spin_lock(&chip->substream_lock);
        chan->count %= chan->frag_size;
    }
}

static irqreturn_t maestro_interrupt(int irq, void *dev_id)
{
    struct maestro *chip = dev_id;
    u8 event;

    if (!chip)
        return IRQ_NONE;

    event = inb(chip->io_base + 0x1A);
    if (!event)
        return IRQ_NONE;

    outw(inw(chip->io_base + 4) & 1, chip->io_base + 4);

    if (event & ESM_HWVOL_IRQ) {
        /* optional: handle hardware volume buttons later */
    }

    /* acknowledge all bits */
    outb(0xFF, chip->io_base + 0x1A);

    if (event & ESM_SOUND_IRQ) {
        struct list_head *p;

        spin_lock(&chip->substream_lock);
        list_for_each(p, &chip->substream_list) {
            struct maestro_pcm_channel *chan =
                list_entry(p, struct maestro_pcm_channel, list);
            maestro_update_pcm(chip, chan);
        }
        spin_unlock(&chip->substream_lock);
    }

    /* SAM / Dream IRQ handling can be added here using SAM_INTERRUPT */

    return IRQ_HANDLED;
}

/* prepare: program hardware registers and reset pointer */

static int maestro_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_pcm_channel *chan = runtime->private_data;

    if (!chan || !chan->mem)
        return -EINVAL;

    chan->hw_ptr = 0;

    /*
     * TODO: Here you must:
     *  - Program WaveCache/APU descriptors to point to chan->mem->addr
     *  - Set buffer/period sizes according to chan->buffer_bytes and chan->period_bytes
     *  - Configure sample format, rate and channels from runtime
     *
     * Use maestro_write() / maestro_read() helpers and descriptor area
     * as per original es1968 / ISIS code.
     */

    return 0;
}

static void maestro_bob_stop(struct maestro *chip)
{
    u16 reg;

    reg = __maestro_read(chip, 0x11);
    reg &= ~0x0001;
    __maestro_write(chip, 0x11, reg);
    reg = __maestro_read(chip, 0x17);
    reg &= ~0x0001;
    __maestro_write(chip, 0x17, reg);
}

static void maestro_bob_start(struct maestro *chip)
{
    int prescale;
    int divide;

    /* compute ideal interrupt frequency for buffer size & play rate */
    for (prescale = 5; prescale < 12; prescale++)
        if (chip->bob_freq > (ESS_SYSCLK >> (prescale + 9)))
            break;

    divide = 1;
    while ((prescale > 5) && (divide < 32)) {
        prescale--;
        divide <<= 1;
    }
    divide >>= 1;

    for (; divide < 31; divide++)
        if (chip->bob_freq >
            ((ESS_SYSCLK >> (prescale + 9)) / (divide + 1)))
            break;

    if (divide == 0) {
        divide++;
        if (prescale > 5)
            prescale--;
    } else if (divide > 1) {
        divide--;
    }

    __maestro_write(chip, 6, 0x9000 | (prescale << 5) | divide);

    __maestro_write(chip, 0x11, __maestro_read(chip, 0x11) | 1);
    __maestro_write(chip, 0x17, __maestro_read(chip, 0x17) | 1);
}

/* call with substream_lock held */
static void maestro_bob_inc(struct maestro *chip, int freq)
{
    int clients = atomic_inc_return(&chip->bobclient);

    if (clients == 1) {
        chip->bob_freq = freq;
        maestro_bob_start(chip);
    } else if (chip->bob_freq < freq) {
        maestro_bob_stop(chip);
        chip->bob_freq = freq;
        maestro_bob_start(chip);
    }
}

/* call with substream_lock held */
static void maestro_bob_dec(struct maestro *chip)
{
    int clients = atomic_dec_return(&chip->bobclient);

    if (clients <= 0) {
        maestro_bob_stop(chip);
        atomic_set(&chip->bobclient, 0);
    } else if (chip->bob_freq > MAESTRO_BOB_FREQ) {
        struct list_head *p;
        int max_freq = MAESTRO_BOB_FREQ;

        list_for_each(p, &chip->substream_list) {
            struct maestro_pcm_channel *chan =
                list_entry(p, struct maestro_pcm_channel, list);
            if (chan->bob_freq > max_freq)
                max_freq = chan->bob_freq;
        }
        if (max_freq != chip->bob_freq) {
            maestro_bob_stop(chip);
            chip->bob_freq = max_freq;
            maestro_bob_start(chip);
        }
    }
}

static int maestro_calc_bob_rate(struct maestro *chip,
                                 struct maestro_pcm_channel *chan,
                                 struct snd_pcm_runtime *runtime)
{
    int freq = runtime->rate * 4;

    if (chan->fmt & ESS_FMT_STEREO)
        freq <<= 1;
    if (chan->fmt & ESS_FMT_16BIT)
        freq <<= 1;

    freq /= chan->frag_size;

    if (freq < MAESTRO_BOB_FREQ)
        freq = MAESTRO_BOB_FREQ;
    else if (freq > MAESTRO_BOB_FREQ_MAX)
        freq = MAESTRO_BOB_FREQ_MAX;

    return freq;
}

static u32 maestro_compute_rate(struct maestro *chip, u32 freq)
{
    u32 rate = (freq << 16) / chip->clock;
    return rate;
}

static inline unsigned int
maestro_get_dma_ptr(struct maestro *chip, struct maestro_pcm_channel *chan)
{
    unsigned int offset;

    offset = apu_get_register(chip, chan->apu[0], 5);
    offset -= chan->base[0];

    return offset & 0xFFFE; /* hardware is in words */
}

static void maestro_apu_set_freq(struct maestro *chip, int apu, int freq)
{
    apu_set_register(chip, apu, 2,
                     (apu_get_register(chip, apu, 2) & 0x00FF) |
                     ((freq & 0xff) << 8) | 0x10);
    apu_set_register(chip, apu, 3, freq >> 8);
}

/* spinlock reg_lock must be held */
static inline void maestro_trigger_apu(struct maestro *chip,
                                       int apu, int mode)
{
    u16 v = apu_get_register(chip, apu, 0);
    v = (v & 0xff0f) | (mode << 4);
    apu_set_register(chip, apu, 0, v);
}

/* simple inline versions using existing apu_set/get */
static inline void maestro_pcm_start(struct maestro *chip,
                                     struct maestro_pcm_channel *chan)
{
    unsigned long flags;

    spin_lock_irqsave(&chip->reg_lock, flags);

    apu_set_register(chip, chan->apu[0], 5, chan->base[0]);
    maestro_trigger_apu(chip, chan->apu[0], chan->apu_mode[0]);

    if (chan->mode == MAESTRO_MODE_CAPTURE) {
        apu_set_register(chip, chan->apu[2], 5, chan->base[2]);
        maestro_trigger_apu(chip, chan->apu[2], chan->apu_mode[2]);
    }

    if (chan->fmt & ESS_FMT_STEREO) {
        apu_set_register(chip, chan->apu[1], 5, chan->base[1]);
        maestro_trigger_apu(chip, chan->apu[1], chan->apu_mode[1]);

        if (chan->mode == MAESTRO_MODE_CAPTURE) {
            apu_set_register(chip, chan->apu[3], 5, chan->base[3]);
            maestro_trigger_apu(chip, chan->apu[3], chan->apu_mode[3]);
        }
    }

    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static inline void maestro_pcm_stop(struct maestro *chip,
                                    struct maestro_pcm_channel *chan)
{
    unsigned long flags;

    spin_lock_irqsave(&chip->reg_lock, flags);

    maestro_trigger_apu(chip, chan->apu[0], 0);
    maestro_trigger_apu(chip, chan->apu[1], 0);
    if (chan->mode == MAESTRO_MODE_CAPTURE) {
        maestro_trigger_apu(chip, chan->apu[2], 0);
        maestro_trigger_apu(chip, chan->apu[3], 0);
    }

    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* programming WaveCache channel control word */
static void maestro_program_wavecache(struct maestro *chip,
                                      struct maestro_pcm_channel *chan,
                                      int channel, u32 addr, int capture)
{
    u32 tmpval = (addr - 0x10) & 0xFFF8;

    if (!capture) {
        if (!(chan->fmt & ESS_FMT_16BIT))
            tmpval |= 4; /* 8-bit */
        if (chan->fmt & ESS_FMT_STEREO)
            tmpval |= 2; /* stereo */
    }

    wave_set_register(chip, chan->apu[channel] << 3, tmpval);
}

/* trigger: start/stop the hardware stream */

static int maestro_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_pcm_channel *chan = runtime->private_data;
    struct maestro *chip = snd_pcm_substream_chip(substream);
    unsigned long flags;

    if (!chan)
        return -EINVAL;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
    case SNDRV_PCM_TRIGGER_RESUME:
    case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
        spin_lock_irqsave(&chan->lock, flags);
        chan->running = 1;
        /*
         * TODO: Enable the corresponding APU/DMA channel in hardware.
         */
        spin_unlock_irqrestore(&chan->lock, flags);
        break;

    case SNDRV_PCM_TRIGGER_STOP:
    case SNDRV_PCM_TRIGGER_SUSPEND:
    case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
        spin_lock_irqsave(&chan->lock, flags);
        chan->running = 0;
        /*
         * TODO: Disable the corresponding APU/DMA channel in hardware.
         */
        spin_unlock_irqrestore(&chan->lock, flags);
        break;

    default:
        return -EINVAL;
    }

    return 0;
}

/* pointer: return current hardware position in frames */

static snd_pcm_uframes_t maestro_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_pcm_channel *chan = runtime->private_data;
    snd_pcm_uframes_t ptr;

    if (!chan || !chan->mem)
        return 0;

    /*
     * TODO: Read current playback/capture position from hardware and convert
     * it to frames. For now, we return the software pointer.
     */
    ptr = chan->hw_ptr;

    if (ptr >= runtime->buffer_size)
        ptr %= runtime->buffer_size;

    return ptr;
}

static const struct snd_pcm_ops maestro_pcm_playback_ops = {
    .open = maestro_pcm_open_playback,
    .close = maestro_pcm_close,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params = maestro_pcm_hw_params,
    .hw_free = maestro_pcm_hw_free,
    .prepare = maestro_pcm_prepare,
    .trigger = maestro_pcm_trigger,
    .pointer = maestro_pcm_pointer,
};

static const struct snd_pcm_ops maestro_pcm_capture_ops = {
    .open = maestro_pcm_open_capture,
    .close = maestro_pcm_close,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params = maestro_pcm_hw_params,
    .hw_free = maestro_pcm_hw_free,
    .prepare = maestro_pcm_prepare,
    .trigger = maestro_pcm_trigger,
    .pointer = maestro_pcm_pointer,
};

/* forward prototypes */
static int maestro_init_dmabuf(struct maestro *chip, int total_kbytes);
static struct maestro_mem_chunk *maestro_new_memory(struct maestro *chip, int size);
static void maestro_free_memory(struct maestro *chip, struct maestro_mem_chunk *buf);
static void maestro_free_dmabuf(struct maestro *chip);

/* ISIS / SAM low-level I/O helpers (ported from maxiinit) */

static void __isis_write_control(struct maestro *chip, u8 data)
{
    unsigned long io = chip->io_base;
    int i = 0;

    outw(0x0001, io + ISIS_ADDRESS);
    /* Wait until not busy (bit 6 cleared) with simple timeout */
    while (((inw(io + ISIS_DATA) & (1 << 6)) != 0) && i++ < 100000)
        cpu_relax();

    outb(data, io + ISIS_DATA);
}

static u8 __isis_read_control(struct maestro *chip)
{
    unsigned long io = chip->io_base;

    outb(0x01, io + ISIS_ADDRESS);
    return inb(io + ISIS_DATA);
}

static void __isis_write_data8(struct maestro *chip, u8 data)
{
    unsigned long io = chip->io_base;
    int i = 0;

    outw(0x0000, io + ISIS_ADDRESS);
    /* Wait until not busy (bit 6 cleared) with simple timeout */
    while (((inw(io + ISIS_DATA) & (1 << 6)) != 0) && i++ < 100000)
        cpu_relax();

    outb(data, io + ISIS_DATA);
}

static u8 __isis_read_data8(struct maestro *chip)
{
    unsigned long io = chip->io_base;

    outb(0x00, io + ISIS_ADDRESS);
    return inb(io + ISIS_DATA);
}

static void __isis_write_data16(struct maestro *chip, u16 data)
{
    unsigned long io = chip->io_base;

    outw(0x0002, io + ISIS_ADDRESS);
    outw(data, io + ISIS_DATA);
}

static u16 __isis_read_data16(struct maestro *chip)
{
    unsigned long io = chip->io_base;

    outb(0x02, io + ISIS_ADDRESS);
    return inw(io + ISIS_DATA);
}

static void __isis_burstwrite_data16(struct maestro *chip,
                                     const u16 *data, u16 length)
{
    unsigned long io = chip->io_base;
    u16 i;

    outw(0x0002, io + ISIS_ADDRESS);
    for (i = 0; i < length; i++)
        outw(data[i], io + ISIS_DATA);
}

static void __isis_burstread_data16(struct maestro *chip,
                                    u16 *buffer, u16 length)
{
    unsigned long io = chip->io_base;
    u16 i;

    outb(0x02, io + ISIS_ADDRESS);
    for (i = 0; i < length; i++)
        buffer[i] = inw(io + ISIS_DATA);
}

/* Public wrappers with timing and locking */

static void isis_write_control(struct maestro *chip, u8 data)
{
    unsigned long flags;

    udelay(ISIS_PRE_WRITE_US);
    spin_lock_irqsave(&chip->reg_lock, flags);
    __isis_write_control(chip, data);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static u8 isis_read_control(struct maestro *chip)
{
    unsigned long flags;
    u8 val;

    udelay(ISIS_PRE_READ_US);
    spin_lock_irqsave(&chip->reg_lock, flags);
    val = __isis_read_control(chip);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
    return val;
}

static void isis_write_data8(struct maestro *chip, u8 data)
{
    unsigned long flags;

    udelay(ISIS_PRE_WRITE_US);
    spin_lock_irqsave(&chip->reg_lock, flags);
    __isis_write_data8(chip, data);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static u8 isis_read_data8(struct maestro *chip)
{
    unsigned long flags;
    u8 val;

    udelay(ISIS_PRE_READ_US);
    spin_lock_irqsave(&chip->reg_lock, flags);
    val = __isis_read_data8(chip);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
    return val;
}

static void isis_write_data16(struct maestro *chip, u16 data)
{
    unsigned long flags;

    spin_lock_irqsave(&chip->reg_lock, flags);
    __isis_write_data16(chip, data);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static u16 isis_read_data16(struct maestro *chip)
{
    unsigned long flags;
    u16 val;

    spin_lock_irqsave(&chip->reg_lock, flags);
    val = __isis_read_data16(chip);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
    return val;
}

static void isis_burstwrite_data16(struct maestro *chip,
                                   const u16 *data, u16 length)
{
    unsigned long flags;

    spin_lock_irqsave(&chip->reg_lock, flags);
    __isis_burstwrite_data16(chip, data, length);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static void isis_burstread_data16(struct maestro *chip,
                                  u16 *buffer, u16 length)
{
    unsigned long flags;

    spin_lock_irqsave(&chip->reg_lock, flags);
    __isis_burstread_data16(chip, buffer, length);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* Wait until SAM control bit 7 is either set (want_set=1) or cleared (want_set=0) */
static int isis_wait_control_bit7(struct maestro *chip, int want_set)
{
    int timeout = 100000;

    while (timeout-- > 0) {
        u8 c = isis_read_control(chip);
        int set = !!(c & (1 << 7));
        if (set == want_set)
            return 0;
        udelay(10);
    }
    dev_err(&chip->pci->dev,
            "maestro: timeout waiting for SAM control bit7=%d\n", want_set);
    return -ETIMEDOUT;
}

static void maestro_playback_setup(struct maestro *chip,
                                   struct maestro_pcm_channel *chan,
                                   struct snd_pcm_runtime *runtime)
{
    u32 pa;
    int high_apu = 0;
    int channel, apu;
    int i, size;
    unsigned long flags;
    u32 freq;

    chan->dma_size = snd_pcm_lib_buffer_bytes(chan->substream);
    chan->frag_size = snd_pcm_lib_period_bytes(chan->substream);

    chan->wav_shift = 1; /* Maestro always uses 16-bit words in DMA engine */
    chan->fmt = 0;
    if (snd_pcm_format_width(runtime->format) == 16)
        chan->fmt |= ESS_FMT_16BIT;
    if (runtime->channels > 1) {
        chan->fmt |= ESS_FMT_STEREO;
        if (chan->fmt & ESS_FMT_16BIT)
            chan->wav_shift++;
    }

    chan->bob_freq = maestro_calc_bob_rate(chip, chan, runtime);

    size = chan->dma_size >> chan->wav_shift;

    if (chan->fmt & ESS_FMT_STEREO)
        high_apu++;

    for (channel = 0; channel <= high_apu; channel++) {
        apu = chan->apu[channel];

        maestro_program_wavecache(chip, chan, channel,
                                  chan->memory->addr, 0);

        pa = chan->memory->addr;
        pa -= chip->dma.addr;
        pa >>= 1; /* words */

        pa |= 0x00400000; /* System RAM bit */

        if (chan->fmt & ESS_FMT_STEREO) {
            if (channel)
                pa |= 0x00800000; /* right channel */
            if (chan->fmt & ESS_FMT_16BIT)
                pa >>= 1;
        }

        chan->base[channel] = pa & 0xFFFF;

        for (i = 0; i < 16; i++)
            apu_set_register(chip, apu, i, 0x0000);

        apu_set_register(chip, apu, 4, ((pa >> 16) & 0xFF) << 8);
        apu_set_register(chip, apu, 5, pa & 0xFFFF);
        apu_set_register(chip, apu, 6, (pa + size) & 0xFFFF);
        apu_set_register(chip, apu, 7, size);

        apu_set_register(chip, apu, 8, 0x0000);
        apu_set_register(chip, apu, 9, 0xD000);
        apu_set_register(chip, apu, 11, 0x0000);
        apu_set_register(chip, apu, 0, 0x400F);

        if (chan->fmt & ESS_FMT_16BIT)
            chan->apu_mode[channel] = 0x01; /* ESM_APU_16BITLINEAR */
        else
            chan->apu_mode[channel] = 0x03; /* ESM_APU_8BITLINEAR */

        if (chan->fmt & ESS_FMT_STEREO) {
            apu_set_register(chip, apu, 10,
                             0x8F00 | (channel ? 0 : 0x10));
            chan->apu_mode[channel] += 1; /* stereo variant */
        } else {
            apu_set_register(chip, apu, 10, 0x8F08);
        }
    }

static int maestro_alloc_apu_pair(struct maestro *chip)
{
    int apu;

    for (apu = 0; apu < NR_APUS; apu += 2) {
        if (!chip->apu[apu] && !chip->apu[apu + 1]) {
            chip->apu[apu] = chip->apu[apu + 1] = 1;
            return apu;
        }
    }
    return -EBUSY;
}

static void maestro_free_apu_pair(struct maestro *chip, int apu)
{
    if (apu < 0 || apu + 1 >= NR_APUS)
        return;
    chip->apu[apu] = chip->apu[apu + 1] = 0;
}
	
	static int maestro_playback_open(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_pcm_channel *chan;
    int apu1;

    apu1 = maestro_alloc_apu_pair(chip);
    if (apu1 < 0)
        return apu1;

    chan = kzalloc(sizeof(*chan), GFP_KERNEL);
    if (!chan) {
        maestro_free_apu_pair(chip, apu1);
        return -ENOMEM;
    }

    chan->apu[0] = apu1;
    chan->apu[1] = apu1 + 1;
    chan->apu_mode[0] = 0;
    chan->apu_mode[1] = 0;
    chan->running = 0;
    chan->substream = substream;
    chan->mode = MAESTRO_MODE_PLAY;

    runtime->private_data = chan;
    runtime->hw.info = SNDRV_PCM_INFO_MMAP |
                       SNDRV_PCM_INFO_MMAP_VALID |
                       SNDRV_PCM_INFO_INTERLEAVED |
                       SNDRV_PCM_INFO_BLOCK_TRANSFER |
                       SNDRV_PCM_INFO_RESUME;
    runtime->hw.formats = SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE;
    runtime->hw.rates = SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000;
    runtime->hw.rate_min = 4000;
    runtime->hw.rate_max = 48000;
    runtime->hw.channels_min = 1;
    runtime->hw.channels_max = 2;
    runtime->hw.buffer_bytes_max = chip->dma.bytes;
    runtime->hw.period_bytes_min = 256;
    runtime->hw.period_bytes_max = chip->dma.bytes;
    runtime->hw.periods_min = 1;
    runtime->hw.periods_max = 1024;

    spin_lock_irq(&chip->substream_lock);
    list_add(&chan->list, &chip->substream_list);
    spin_unlock_irq(&chip->substream_lock);

    return 0;
}

static int maestro_playback_close(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct maestro_pcm_channel *chan;

    if (!substream->runtime->private_data)
        return 0;

    chan = substream->runtime->private_data;

    spin_lock_irq(&chip->substream_lock);
    list_del(&chan->list);
    spin_unlock_irq(&chip->substream_lock);

    maestro_free_apu_pair(chip, chan->apu[0]);
    kfree(chan);

    return 0;
}

    spin_lock_irqsave(&chip->reg_lock, flags);
    outw(1, chip->io_base + 0x04); /* clear WaveProcessor ints */
    outw(inw(chip->io_base + ESM_PORT_HOST_IRQ) | ESM_HIRQ_DSIE,
         chip->io_base + ESM_PORT_HOST_IRQ);
    spin_unlock_irqrestore(&chip->reg_lock, flags);

    freq = runtime->rate;
    if (freq > 48000)
        freq = 48000;
    if (freq < 4000)
        freq = 4000;

    if (!(chan->fmt & ESS_FMT_16BIT) && !(chan->fmt & ESS_FMT_STEREO))
        freq >>= 1;

    freq = maestro_compute_rate(chip, freq);

    maestro_apu_set_freq(chip, chan->apu[0], freq);
    maestro_apu_set_freq(chip, chan->apu[1], freq);

}

/* -----------------------
   Low level index/data helpers (copied/adapted from es1968)
   ----------------------- */

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

static inline void __maestro_write(struct maestro *chip, u16 reg, u16 val)
{
    outw(reg, chip->io_base + ESM_INDEX);
    outw(val, chip->io_base + ESM_DATA);
}

static void maestro_write(struct maestro *chip, u16 reg, u16 val)
{
    unsigned long flags;
    spin_lock_irqsave(&chip->reg_lock, flags);
    __maestro_write(chip, reg, val);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* AC97 wait helper (bus not busy) */
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

static void maestro_ac97_write(struct snd_ac97 *ac97, unsigned short reg, unsigned short val)
{
    struct maestro *chip = ac97->private_data;
    unsigned long flags;
    spin_lock_irqsave(&chip->reg_lock, flags);
    snd_maestro_ac97_wait(chip);
    outw(val, chip->io_base + ESM_AC97_DATA);
    outb(reg, chip->io_base + ESM_AC97_INDEX);
    mdelay(1);
    spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static unsigned short maestro_ac97_read(struct snd_ac97 *ac97, unsigned short reg)
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

static struct snd_ac97_bus_ops maestro_ac97_bus_ops = {
    .write = maestro_ac97_write,
    .read  = maestro_ac97_read,
};

/* -------------------------
   DMA pool behaviour (es1968 exact port)
   ------------------------- */

/* Free the reserved DMA pool and free chunk list */
static void maestro_free_dmabuf(struct maestro *chip)
{
    struct list_head *p, *n;
    if (!chip->dma.area)
        return;

    /* free reserved DMA pages (ALSA helper) */
    snd_dma_free_reserved(&chip->dma_dev);

    /* free list of chunks */
    list_for_each_safe(p, n, &chip->buf_list) {
        struct maestro_mem_chunk *chunk = list_entry(p, struct maestro_mem_chunk, list);
        list_del(&chunk->list);
        kfree(chunk);
    }
    chip->dma.area = NULL;
}

/*
   Initialize reserved DMA pool.

   total_kbytes: requested total size in kilobytes (like es1968 total_bufsize) */
static int maestro_init_dmabuf(struct maestro *chip, int total_kbytes)
{
    struct maestro_mem_chunk *chunk;

    /* Setup DMA device context for PCI */
    snd_dma_device_pci(&chip->dma_dev, chip->pci, 0);

    /* try to get a previously reserved DMA area */
    if (!snd_dma_get_reserved(&chip->dma_dev, &chip->dma)) {
        /* Not reserved: allocate fallback pages (ALSA helper), returns virtual pointer */
        chip->dma.area = snd_malloc_pci_pages_fallback(chip->pci, total_kbytes << 10,
                                                       &chip->dma.addr, &chip->dma.bytes);
        if (!chip->dma.area) {
            dev_err(&chip->pci->dev, "maestro: can't allocate dma pages for size %d kB\n", total_kbytes);
            return -ENOMEM;
        }
        /* Ensure within 28-bit window (WaveCache limitation) */
        if ((chip->dma.addr + chip->dma.bytes - 1) & ~((1 << 28) - 1)) {
            snd_dma_free_pages(&chip->dma);
            dev_err(&chip->pci->dev, "maestro: DMA buffer beyond 256MB (28-bit limit)\n");
            return -ENOMEM;
        }
        snd_dma_set_reserved(&chip->dma_dev, &chip->dma);
    }

    /* Init chunk list */
    INIT_LIST_HEAD(&chip->buf_list);

    /* Create an initial free chunk (skip first 512 bytes as es1968 did) */
    chunk = kmalloc(sizeof(*chunk), GFP_KERNEL);
    if (!chunk) {
        maestro_free_dmabuf(chip);
        return -ENOMEM;
    }
    memset(chip->dma.area, 0, 512);
    chunk->buf = chip->dma.area + 512;
    chunk->addr = chip->dma.addr + 512;
    chunk->size = chip->dma.bytes - 512;
    chunk->empty = 1;
    list_add_tail(&chunk->list, &chip->buf_list);

    chip->total_bufsize_kb = total_kbytes;
    return 0;
}

/* Allocate a chunk from the reserved pool (es1968 semantics: first-fit, split) */
static struct maestro_mem_chunk *maestro_new_memory(struct maestro *chip, int size)
{
    struct list_head *p;
    struct maestro_mem_chunk *chunk;

    if (!size)
        return NULL;

    /* size must be multiple of 64 bytes (WaveCache requirement) */
    size = (size + 63) & ~63;

    list_for_each(p, &chip->buf_list) {
        chunk = list_entry(p, struct maestro_mem_chunk, list);
        if (chunk->empty && chunk->size >= size) {
            if (chunk->size > size) {
                /* split */
                struct maestro_mem_chunk *newc;
                newc = kmalloc(sizeof(*newc), GFP_ATOMIC);
                if (!newc)
                    return NULL;
                newc->buf = chunk->buf + size;
                newc->addr = chunk->addr + size;
                newc->size = chunk->size - size;
                newc->empty = 1;
                chunk->size = size;
                list_add(&newc->list, &chunk->list);
            }
            chunk->empty = 0;
            return chunk;
        }
    }
    return NULL;
}

/* Free a chunk back to the pool and coalesce neighbours */
static void maestro_free_memory(struct maestro *chip, struct maestro_mem_chunk *buf)
{
    struct list_head *p;
    struct maestro_mem_chunk *chunk;

    if (!buf)
        return;
    buf->empty = 1;

    /* coalesce with next */
    p = buf->list.next;
    if (p != &chip->buf_list) {
        chunk = list_entry(p, struct maestro_mem_chunk, list);
        if (chunk->empty) {
            buf->size += chunk->size;
            list_del(&chunk->list);
            kfree(chunk);
        }
    }

    /* coalesce with prev */
    p = buf->list.prev;
    if (p != &chip->buf_list) {
        chunk = list_entry(p, struct maestro_mem_chunk, list);
        if (chunk->empty) {
            chunk->size += buf->size;
            list_del(&buf->list);
            kfree(buf);
        }
    }

static int maestro_hw_params(struct snd_pcm_substream *substream,
                             struct snd_pcm_hw_params *hw_params)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_pcm_channel *chan = runtime->private_data;
    int size = params_buffer_bytes(hw_params);

    if (chan->memory) {
        if (chan->memory->size >= size) {
            runtime->dma_bytes = size;
            return 0;
        }
        maestro_free_memory(chip, chan->memory);
        chan->memory = NULL;
    }

    chan->memory = maestro_new_memory(chip, size);
    if (!chan->memory)
        return -ENOMEM;

    runtime->dma_area = chan->memory->buf;
    runtime->dma_addr = chan->memory->addr;
    runtime->dma_bytes = size;

    return 0;
}

static int maestro_hw_free(struct snd_pcm_substream *substream)
{
    struct maestro *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct maestro_pcm_channel *chan = runtime->private_data;

    if (!chan)
        return 0;

    if (chan->memory) {
        maestro_free_memory(chip, chan->memory);
        chan->memory = NULL;
    }
    return 0;
}

	
}

/* -------------------------
   Higher-level driver logic (PCM, AC97, irq, probe)
   ------------------------- */

/* simplified playback/capture skeleton omitted here for brevity; the DMA pool
   behaviour above is the key port requested by the user. The rest of the
   driver uses the same index/data, wave and ac97 helpers already ported. */

/* For demonstration, we keep the earlier minimal implementations of the rest
   of the driver that call into the DMA pool routines where appropriate. */

static irqreturn_t maestro_interrupt(int irq, void *dev_id)
{
    struct maestro *chip = dev_id;
    u16 event;

    if (!chip)
        return IRQ_NONE;

    event = inw(chip->io_base + ESM_PORT_HOST_IRQ);
    if (!event)
        return IRQ_NONE;

    outw(inw(chip->io_base + 4) & 1, chip->io_base + 4);
    outb(0xFF, chip->io_base + 0x1A);

        if (event & ESM_HIRQ_DSIE) {
        struct list_head *p, *n;

        spin_lock(&chip->substream_lock);
        list_for_each_safe(p, n, &chip->substream_list) {
            struct maestro_pcm_channel *chan =
                list_entry(p, struct maestro_pcm_channel, list);

            /*
             * TODO: Check which APU/channel raised the interrupt, update chan->hw_ptr
             * according to hardware position and call:
             *
             * snd_pcm_period_elapsed(chan->substream);
             */
        }
        spin_unlock(&chip->substream_lock);
    }

    return IRQ_HANDLED;
}

/* AC97 attach using modern snd_ac97_bus() API */
static int maestro_ac97_attach(struct maestro *chip)
{
    struct snd_ac97_bus *bus;
    struct snd_ac97_template ac97;
    int err;

    err = snd_ac97_bus(chip->card, 0, &maestro_ac97_bus_ops, chip, &bus);
    if (err < 0)
        return err;

    memset(&ac97, 0, sizeof(ac97));
    ac97.private_data = chip;
    ac97.pci = chip->pci;
    ac97.scaps = AC97_SCAP_AUDIO;

    err = snd_ac97_mixer(bus, &ac97, &chip->ac97);
    if (err < 0) {
        chip->ac97 = NULL;
        return err;
    }

    return 0;
}

static void maestro_cleanup_ac97(struct maestro *chip)
{
    /* AC97 codec is released automatically with the card */
}

static const u16 samBoot[] = {
    0xD0CE,0x0111,0xD0CE,0x01D5,0x0001,0x0003,0x0004,0x0006,
    0x0001,0x0003,0x0002,0x0002,0x0006,0x0002,0x0001,0x0006,
    0x0006,0x7A0C,0xE628,0x0001,0xD448,0x1010,0xC4CB,0xD1CB,
    0xE2FE,0x4F01,0xE3FC,0x4E0D,0xE0FA,0x4700,0x8407,0xD148,
    0x0104,0x9107,0x7A08,0x7A09,0xC590,0xD1CB,0xE2FE,0x4F01,
    0xE3EE,0xC74D,0x6DFA,0xD44A,0x012E,0xC449,0x7816,0x7819,
    0x7821,0x781D,0x782E,0x7830,0x7835,0x783A,0x783F,0x7849,
    0x784C,0x786E,0x786D,0x7914,0x01F8,0x7A10,0x7A11,0x7915,
    0x0000,0x7913,0x0007,0x7A12,0xD1CA,0xC44F,0xC4C4,0xD0CE,
    0x01CB,0xC64F,0xC54F,0xC44F,0xCB4C,0xD5C4,0x78C4,0xC64F,
    0xC74F,0xCF4C,0xC64F,0xC54F,0xCB4C,0x3D09,0xC64F,0xC54F,
    0xCB4C,0x3D08,0xD449,0x0130,0xE302,0xC480,0x786C,0xCF80,
    0x78B2,0xC04F,0xC4C9,0x7867,0xC64F,0xC54F,0xCB4C,0xC04F,
    0xC5CB,0x78A9,0xC54F,0xC44F,0xC94A,0xD1CE,0x8405,0x785B,
    0xC54F,0xC44F,0xC94A,0xD1CE,0x8406,0x7855,0xC74F,0xC64F,
    0xCD4E,0xC74F,0xC54F,0xCB4E,0xC74F,0xC44F,0xC94E,0xD1CF,
    0x7892,0xC64F,0xC54F,0xCB4C,0xC549,0xC04F,0x0001,0x0400,
    0xC4CB,0x0115,0x0406,0xC04F,0xD0C1,0x7D01,0x6CFC,0xD0CA,
    0x8418,0x0001,0xC4CB,0xD1C9,0x0001,0x840C,0xE901,0x0000,
    0x7803,0xE911,0xD048,0xFFFF,0x7B00,0xE920,0xD1C8,0xC04F,
    0xC14F,0xC24F,0xC34F,0xC44F,0xC54F,0xC64F,0xC74F,0xD1CA,
    0x8704,0x0001,0x0410,0xC4CB,0xC54F,0xC44F,0xC94A,0x3C0D,
    0xC54F,0xC44F,0xC94A,0x3C0F,0xC54F,0xC44F,0xC94A,0x3C0E,
    0x0001,0xD448,0x2010,0xD548,0x3010,0xD749,0x013A,0xE304,
    0xD448,0x1010,0xD548,0x1010,0xC4CB,0xC5CB,0x0006,0xC4CB,
    0x7B0D,0xE3FE,0x78B5,0x0006,0xC4CB,0x0001,0xC5C9,0x3510,
    0xE2FD,0xCA49,0x0006,0xC5CB,0x78AB,0xC74D,0xC64D,0xC54D,
    0xC44D,0xC34D,0xC24D,0xC14D,0xC04D,0x7A05,0x840C,0x4100,
    0xE101,0x4104,0xE301,0x4201,0xE501,0x4302,0xD94A,0xD94B,
    0x3C0C,0x0001,0x0400,0xC4CB,0xD0C8,0x0110,0x0406,0xC0C1,
    0xC04D,0x7C01,0x6CFC,0xD0CF,0x013B,0xD448,0x55AA,0x78D3,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000
};

/* Upload ISIS SAM firmware (pci64.bin) and configure SAM, based on maxiinit */ 
static int maestro_upload_firmware(struct maestro *chip)
{
    const struct firmware *fw = NULL;
    int err;
    u16 w;
    unsigned long io = chip->io_base;
    u16 *fw_words = NULL;
    size_t fw_size, payload_bytes, payload_words;
    int i;
    u8 resp;

    dev_info(&chip->pci->dev, "maestro: ISIS SAM init started\n");

    /* Disable SAM interrupt (Host Interrupt Control) */
    w = inw(io + ESM_PORT_HOST_IRQ);
    w &= ~SAM_INTERRUPT;
    outw(w, io + ESM_PORT_HOST_IRQ);

    /* Enable MPU-401 decode (Config A / PCI offset 0x50, OR 0x18) */
    pci_read_config_word(chip->pci, ESM_CONFIG_A, &w);
    w |= 0x0018;
    pci_write_config_word(chip->pci, ESM_CONFIG_A, w);

    /* Clock source setup (GPIO mask, direction, data) */
    outw(0x0193, io + ESM_GPIO_MASK);
    outw(0x0E64, io + ESM_GPIO_DIR);
    w = inw(io + ESM_GPIO_DATA);
    w &= 0xFF9F;
    w |= 0x0024;
    outw(w, io + ESM_GPIO_DATA);

    /* Mysterious PLD sequence (mask + data bit 9 set) */
    outw(0x0DFF, io + ESM_GPIO_MASK);
    w = inw(io + ESM_GPIO_DATA);
    w |= 0x0200;
    outw(w, io + ESM_GPIO_DATA);
    outw(0x0FFF, io + ESM_GPIO_MASK);

    /* Reset SAM (control 0x70, data 0x11) */
    isis_write_control(chip, 0x70);
    isis_write_data8(chip, 0x11);
    msleep(10);

    /* Boot SAM with samBoot[] code */
    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;

    isis_burstwrite_data16(chip, samBoot,
                           (u16)(ARRAY_SIZE(samBoot)));

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;

    /* Finish boot: control 0x04, then 0x00 */
    isis_write_control(chip, 0x04);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;

    isis_write_control(chip, 0x00);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;

    /* Firmware load preparation (sequence of control writes) */
    isis_write_control(chip, 0x05);
    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x00);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x00);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x00);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x0B);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x00);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x02);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x00);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x00);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x57);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x6B);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;

    msleep(10);

    /* Load pci64.bin via request_firmware */
    err = request_firmware(&fw, "pci64.bin", &chip->pci->dev);
    if (err < 0) {
        dev_err(&chip->pci->dev,
                "maestro: cannot load firmware pci64.bin (%d)\n", err);
        return err;
    }

    fw_size = fw->size;
    if (fw_size <= 0x400) {
        dev_err(&chip->pci->dev,
                "maestro: firmware too small (%zu bytes)\n", fw_size);
        err = -EINVAL;
        goto out_release_fw;
    }

    payload_bytes = fw_size - 0x400;
    payload_words = payload_bytes / 2;

    fw_words = kmalloc(payload_words * sizeof(u16), GFP_KERNEL);
    if (!fw_words) {
        err = -ENOMEM;
        goto out_release_fw;
    }

    /* Copy and align firmware payload as 16-bit words */
    memcpy(fw_words, fw->data + 0x400, payload_words * sizeof(u16));

    dev_info(&chip->pci->dev,
             "maestro: uploading firmware (%zu words)\n", payload_words);

    isis_burstwrite_data16(chip, fw_words, (u16)payload_words);

    kfree(fw_words);
    fw_words = NULL;
    release_firmware(fw);
    fw = NULL;

    /* Post-firmware mysterious sequence: 0x09, 0x00, 0x02 */
    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x09);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x00);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x02);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;

    /* Disable SAM interrupt again */
    w = inw(io + ESM_PORT_HOST_IRQ);
    w &= ~SAM_INTERRUPT;
    outw(w, io + ESM_PORT_HOST_IRQ);

    /* Switch to UART mode (control 0x3F) */
    isis_write_control(chip, 0x3F);
    msleep(10);

    /* Wait for SAM response 0xFE on DATA8 */
    err = isis_wait_control_bit7(chip, 0);
    if (err < 0)
        return err;

    resp = isis_read_data8(chip);
    if (resp != 0xFE)
        dev_warn(&chip->pci->dev,
                 "maestro: unexpected SAM UART response 0x%02x\n", resp);

    /* Get MMT address (control 0x03, param 0) */
    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x03);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_data8(chip, 0x00);

    for (i = 0; i < 4; i++) {
        err = isis_wait_control_bit7(chip, 0);
        if (err < 0)
            return err;
        chip->MMT_addr[i] = isis_read_data8(chip);
    }

    /* More mysterious control sequences: 0x05 -> 0x01, 0x2C -> 0x00 */
    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x05);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_data8(chip, 0x01);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_control(chip, 0x2C);

    err = isis_wait_control_bit7(chip, 1);
    if (err < 0)
        return err;
    isis_write_data8(chip, 0x00);

    /* Test interrupt generation (control 0x48, data 0x00) */
    isis_write_control(chip, 0x48);
    isis_write_data8(chip, 0x00);

    err = isis_wait_control_bit7(chip, 0);
    if (err < 0)
        return err;

    resp = isis_read_data8(chip);
    if (resp != 0x88)
        dev_warn(&chip->pci->dev,
                 "maestro: unexpected SAM interrupt test response 0x%02x\n",
                 resp);

    /* Unmute output channels via GPIO: mask 0x07FF, set bit 11, then mask 0x0FFF */
    outw(0x07FF, io + ESM_GPIO_MASK);
    w = inw(io + ESM_GPIO_DATA);
    w |= (1 << 11);
    outw(w, io + ESM_GPIO_DATA);
    outw(0x0FFF, io + ESM_GPIO_MASK);

    dev_info(&chip->pci->dev, "maestro: ISIS SAM init complete\n");
    return 0;

out_release_fw:
    if (fw_words)
        kfree(fw_words);
    if (fw)
        release_firmware(fw);
    return err;
}

/* Basic chip init after PCI and IO regions are set up */
static void maestro_chip_init(struct maestro *chip)
{
    u16 w;
    unsigned long iobase = chip->io_base;

    w = inw(iobase + ESM_LEGACY_AUDIO_CONTROL);
    w &= ~0x000f;
    w |= 0x0001;
    outw(w, iobase + ESM_LEGACY_AUDIO_CONTROL);

    w = inw(iobase + ESM_PORT_HOST_IRQ);
    w &= ~(ESM_HIRQ_DSIE | ESM_HIRQ_MPU401 | ESM_HIRQ_HW_VOLUME | SAM_INTERRUPT);
    outw(w, iobase + ESM_PORT_HOST_IRQ);
}

/* PCI probe / remove */

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
    INIT_LIST_HEAD(&chip->buf_list);
    INIT_LIST_HEAD(&chip->substream_list);
    atomic_set(&chip->bobclient, 0);
    atomic_set(&chip->running, 0);
    chip->bob_freq = MAESTRO_BOB_FREQ;

    chip->pci = pdev;
    pci_set_drvdata(pdev, chip);

    bar0_start = pci_resource_start(pdev, MAESTRO_BAR0);
    bar0_len = pci_resource_len(pdev, MAESTRO_BAR0);
    if (!request_region(bar0_start, bar0_len, DRIVER_NAME)) {
        dev_err(&pdev->dev, "maestro: cannot reserve I/O region\n");
        err = -EBUSY;
        goto err_free;
    }

    chip->io_base = bar0_start;

    err = request_irq(pdev->irq, maestro_interrupt, IRQF_SHARED, DRIVER_NAME, chip);
    if (err) {
        dev_err(&pdev->dev, "maestro: cannot request IRQ %d\n", pdev->irq);
        goto err_region;
    }
    chip->irq = pdev->irq;

    err = snd_card_new(&pdev->dev, -1, NULL, THIS_MODULE, 0, &card);
    if (err < 0)
        goto err_irq;

    chip->card = card;

    maestro_chip_init(chip);

    /* create PCM and AC97 */

	struct snd_pcm *pcm;
	err = snd_pcm_new(card, "Maestro PCM", 0, 1, 1, &pcm);
	if (err)
		goto err_card;
	chip->pcm = pcm;
	pcm->private_data = chip;
	pcm->info_flags = 0;
	strcpy(pcm->name, "Guillemot Maxi Studio ISIS");

    /* Initialize reserved DMA pool (e.g. 1024 kB = 1 MB) */
    err = maestro_init_dmabuf(chip, 1024);
    if (err)
        dev_warn(&pdev->dev,
                 "maestro: DMA pool init failed %d (continuing, no PCM)\n",
                 err);

    err = snd_card_new(&pdev->dev, -1, "maestro", THIS_MODULE, 0, &card);
    if (err)
        goto err_irq;
    chip->card = card;
    card->private_data = chip;

    maestro_chip_init(chip);

    /* full PCM: playback only for now */
    if (!err) {
        struct snd_pcm *pcm;
        err = snd_pcm_new(card, "Maestro PCM", 0, 1, 0, &pcm);
        if (err)
            goto err_card;
        chip->pcm = pcm;
        snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
                        &maestro_playback_ops);
        strcpy(pcm->name, "ESS Maestro PCM");
    }

	/* AC'97 Inizialization */
	err = maestro_ac97_attach(chip);
	if (err < 0) {
		dev_err(&pdev->dev, "maestro: AC97 attach failed (%d)\n", err);
		goto err_card;
	}

	/* DMA pool inizialization (dimension: 512 kB).
	 * total_kbytes can be changed by needs. */
	err = maestro_init_dmabuf(chip, 512);
	if (err < 0) {
		dev_err(&pdev->dev, "maestro: DMA pool init failed (%d)\n", err);
		goto err_card;
	}

	/* ISIS firmware upload */
	err = maestro_upload_firmware(chip);
	if (err < 0) {
		dev_err(&pdev->dev, "maestro: firmware upload failed (%d)\n", err);
		goto err_card;
	}

	/* Metadata */
	strcpy(card->driver, DRIVER_NAME);
	strcpy(card->shortname, "Guillemot Maxi Studio ISIS");
	snprintf(card->longname, sizeof(card->longname),
			 "%s at 0x%lx, irq %d",
			 card->shortname, chip->io_base, chip->irq);

	err = snd_card_register(card);
	if (err < 0)
		goto err_card;

	dev_info(&pdev->dev, "maestro: Guillemot Maxi Studio ISIS initialized\n");
	return 0;

	err_card:
			snd_card_free(card);
	err_irq:
			free_irq(chip->irq, chip);
	err_region:
			release_region(chip->io_base, pci_resource_len(pdev, MAESTRO_BAR0));
	err_free:
			kfree(chip);
			pci_set_drvdata(pdev, NULL);
	err_disable:
			pci_disable_device(pdev);
			return err;
}

/* PCI device remove */
static void maestro_remove(struct pci_dev *pdev)
{
    struct maestro *chip = pci_get_drvdata(pdev);

    if (!chip)
        return;

    /* Interrupt disabling */
    outw(0, chip->io_base + ESM_PORT_HOST_IRQ);

    free_irq(chip->irq, chip);

    /* Reserved DMA pool flush */
    maestro_free_dmabuf(chip);

    /* Automatic AC97 freeup by snd_card_free */
    if (chip->card)
        snd_card_free(chip->card);

    release_region(chip->io_base, pci_resource_len(pdev, MAESTRO_BAR0));

    pci_set_drvdata(pdev, NULL);
    pci_disable_device(pdev);

    kfree(chip);
}

/* PCI ID Table */
static const struct pci_device_id maestro_ids[] = {
    { PCI_DEVICE(MAESTRO_VENDOR, MAESTRO_DEVICE) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, maestro_ids);

/* PCI driver structure */
static struct pci_driver maestro_driver = {
    .name       = DRIVER_NAME,
    .id_table   = maestro_ids,
    .probe      = maestro_probe,
    .remove     = maestro_remove,
};

static int __init maestro_init(void)
{
    return pci_register_driver(&maestro_driver);
}

static void __exit maestro_exit(void)
{
    pci_unregister_driver(&maestro_driver);
}

module_init(maestro_init);
module_exit(maestro_exit);

MODULE_AUTHOR("Port/cleanup per Guillemot Maxi Studio ISIS");
MODULE_DESCRIPTION("ALSA driver per Guillemot Maxi Studio ISIS (Maestro2-based)");
MODULE_LICENSE("GPL");
