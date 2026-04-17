// SPDX-License-Identifier: GPL-2.0-only
/*
 * maestro2em.c  —  ALSA driver for Guillemot Maxi Studio ISIS
 *                  ESS Maestro-2E (ES1978) audio engine
 *
 * PCI ID: 0x125D:0x1978
 *
 * ─── Project ─────────────────────────────────────────────────────────────
 * ISISALSA — open-source Linux ALSA driver for the Guillemot Maxi Studio
 * ISIS soundcard.  This file covers the ESS Maestro-2E audio subsystem.
 * SAM9707 wavetable synthesis (isis.bin firmware) is handled separately.
 *
 * ─── Sources used ────────────────────────────────────────────────────────
 *  [1] maestro.c v0.14 (Alan Cox / Zach Brown, 2000-01-28) — OSS reference
 *  [2] ESMGR.VXD / ES197X.DRV — Win9x binary analysis (ESMGR Object 4/5)
 *  [3] ISIS schematic v0.7 (Jimmy Le Rhun, GPL, 2003)
 *  [4] isis.bin / PCI64.BIN — firmware image analysis
 *  [5] ESS Maestro-2 datasheet (104T31)
 *
 * ─── Architecture summary (schematic v0.7) ───────────────────────────────
 *  Maestro-2E (ES1978MS): PCI audio accelerator.
 *    64 APUs, WaveProcessor, WaveCache (28-bit DMA).
 *    AC97 codec bridge → PCM3001E codec on mainboard.
 *    Digital audio output (MAESTRO_DOUT) → PCM1718E DAC.
 *    GPIO: amp-mute, clock-source, PLD control, HW-volume wheel.
 *
 *  SAM9707 (Dream): wavetable synthesis co-processor (separate module).
 *    Connected via ISA-style bus bridged by Maestro GPIO/ISIS ports.
 *    Programmed by uploading isis.bin after samBoot bootstrap.
 *
 * ─── Known limitations ───────────────────────────────────────────────────
 *  · Hardware volume wheel — ISR stub, not yet exposed as ALSA control.
 *  · S/PDIF I/O (CS8414/CS8402 on external box) — not yet supported.
 *  · SAM9707 MIDI/wavetable — separate module (isis_sam.c, TODO).
 *  · Suspend/resume — not implemented.
 *  · Capture locked to stereo 16-bit; mono/8-bit record TODO.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/io.h>
#include <linux/ioport.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/ac97_codec.h>
#include <sound/control.h>

/* =========================================================================
 * Constants
 * ======================================================================= */

#define DRIVER_NAME      "maestro2em"
#define DRIVER_VERSION   "0.3"
#define MAESTRO_VENDOR   0x125D
#define MAESTRO_DEVICE   0x1978

/* ── BAR0 I/O port offsets ──────────────────────────────────────────────── */

/* WaveProcessor indirect register bus */
#define ESM_DATA         0x00   /* WP data                          */
#define ESM_INDEX        0x02   /* WP index                         */

/* WP CRAM double-indirection (BUG-1) */
#define IDR0_DATA_PORT   0x00   /* CRAM data port (via WP index)    */
#define IDR1_CRAM_PTR    0x01   /* CRAM address pointer             */
#define IDR2_CRAM_DATA   0x02
#define IDR6_TIMER_CTRL  0x06
#define IDR7_WAVE_ROMRAM 0x07

/* WaveCache direct ports (BUG-2) */
#define WC_INDEX         0x10   /* WaveCache register select        */
#define WC_DATA          0x12   /* WaveCache data                   */
#define WC_CONTROL       0x14   /* WaveCache size/enable control    */

/* AC97 codec bridge */
#define ESM_AC97_INDEX   0x30
#define ESM_AC97_DATA    0x32

/* Ring bus control */
#define RING_BUS_CTRL_L  0x34
#define RING_BUS_CTRL_H  0x36

/* Host interrupt control */
#define ESM_HOST_IRQ     0x18
#define ESM_IRQ_STATUS   0x1A

/* GPIO (schematic: amp-mute on GPIO[11], clock-sel on GPIO[5:4]) */
#define ESM_GPIO_DATA    0x60
#define ESM_GPIO_MASK    0x64
#define ESM_GPIO_DIR     0x68

/* Host IRQ bits */
#define ESM_HIRQ_DSIE       BIT(2)
#define ESM_HIRQ_MPU401     BIT(1)
#define ESM_HIRQ_HW_VOLUME  BIT(6)

/* IRQ status bits */
#define ESM_SOUND_IRQ    0x04
#define ESM_HWVOL_IRQ    0x40

/* PCI config offsets */
#define PCI_CFG_MAESTRO_A  0x50
#define PCI_CFG_MAESTRO_B  0x52
#define PCI_CFG_LEGACY_CTL 0x40

/* ── Hardware constants ─────────────────────────────────────────────────── */

#define NR_APUS          64
#define NR_APU_REGS      16

/*
 * BUG-3 fix: correct clock reference for Maestro-2E.
 * 50 MHz oscillator divided by 1024 internal prescaler = 48828 Hz.
 * This is used in compute_rate() to generate APU frequency registers.
 * Source: clock_freq[TYPE_MAESTRO2E] = 50000000L/1024L in reference [1].
 */
#define MAESTRO_CLOCK    (50000000L / 1024L)   /* 48828 Hz */
#define ESS_SYSCLK       50000000UL            /* Bob timer oscillator */

/* Stream format flags */
#define ESS_FMT_STEREO   0x01
#define ESS_FMT_16BIT    0x02

#define MAESTRO_MODE_PLAY    0
#define MAESTRO_MODE_CAPTURE 1

/* Bob timer frequency bounds (from reference [1] BOB_MIN/BOB_MAX) */
#define MAESTRO_BOB_FREQ     200
#define MAESTRO_BOB_FREQ_MAX 800

/* DMA pool: first 512 bytes reserved by hardware status FIFO */
#define DMA_POOL_RSVD    512

/*
 * WaveCache alignment: all DMA allocations must be aligned to 64 bytes
 * (WaveCache addressing granularity).
 */
#define WC_ALIGN         64

/*
 * Capture mixbuf: 512-byte intermediate buffer per channel pair.
 * The InputMixer APU writes here; the SRC APU reads from here.
 * Reference [1]: ess->mixbuf = rawbuf + (512 * (i+1)).
 */
#define MIXBUF_SIZE      512

/* =========================================================================
 * Data structures
 * ======================================================================= */

/**
 * struct maestro_chunk - one allocation unit inside the DMA pool
 * @buf:   kernel virtual address
 * @addr:  bus (DMA) address
 * @size:  byte length
 * @empty: 1 = free
 * @list:  linkage in maestro.buf_list
 */
struct maestro_chunk {
	char            *buf;
	dma_addr_t       addr;
	int              size;
	int              empty;
	struct list_head list;
};

/**
 * struct maestro - per-card chip context
 */
struct maestro {
	struct snd_card *card;
	struct pci_dev  *pci;
	unsigned long    io_base;
	int              irq;

	/* ── DMA pool ──────────────────────────────────────────────────── */
	void            *dma_area;
	dma_addr_t       dma_addr;
	size_t           dma_bytes;
	struct list_head buf_list;

	/* ── ALSA objects ──────────────────────────────────────────────── */
	struct snd_pcm  *pcm;
	struct snd_ac97 *ac97;

	/* ── Locking ───────────────────────────────────────────────────── */
	spinlock_t reg_lock;        /* WP/WC register bus             */
	spinlock_t substream_lock;  /* active-stream list + bob timer */

	/* ── APU allocation bitmap ────────────────────────────────────── */
	u8 apu[NR_APUS];

	/* ── Bob timer ────────────────────────────────────────────────── */
	atomic_t bobclient;
	int      bob_freq;

	/* ── Sample-rate reference clock (BUG-3) ─────────────────────── */
	int clock;   /* = MAESTRO_CLOCK = 48828 Hz */

	/* ── Active PCM channels ──────────────────────────────────────── */
	struct list_head substream_list;
};

/**
 * struct maestro_channel - per-stream state
 *
 * Playback:  apu[0]=L play, apu[1]=R play
 * Capture:   apu[0]=L SRC,  apu[1]=R SRC,
 *            apu[2]=L InMix, apu[3]=R InMix
 */
struct maestro_channel {
	struct maestro            *chip;
	struct snd_pcm_substream  *substream;

	int running;
	int mode;

	u8  apu[4];
	u8  apu_mode[4];
	u16 base[4];

	struct maestro_chunk *memory;   /* main DMA buffer       */
	struct maestro_chunk *mixbuf;   /* capture intermediate  */

	unsigned char   fmt;        /* ESS_FMT_* flags           */
	unsigned int    wav_shift;  /* word→byte shift            */

	unsigned int    hwptr;
	unsigned int    count;
	unsigned int    dma_size;
	unsigned int    frag_size;

	int             bob_freq;

	spinlock_t       lock;
	struct list_head list;
};

/* =========================================================================
 * WaveProcessor register helpers
 *
 * These are the "outer" WP indirect registers accessed via ESM_INDEX /
 * ESM_DATA (BAR0+0x02 / BAR0+0x00).  All __* variants are unlocked;
 * callers must hold chip->reg_lock.
 * ======================================================================= */

static inline u16 __wp_read(struct maestro *chip, u16 reg)
{
	outw(reg, chip->io_base + ESM_INDEX);
	return inw(chip->io_base + ESM_DATA);
}

static inline void __wp_write(struct maestro *chip, u16 reg, u16 val)
{
	outw(reg, chip->io_base + ESM_INDEX);
	outw(val, chip->io_base + ESM_DATA);
}

static u16 wp_read(struct maestro *chip, u16 reg)
{
	unsigned long flags;
	u16 v;

	spin_lock_irqsave(&chip->reg_lock, flags);
	v = __wp_read(chip, reg);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return v;
}

static void wp_write(struct maestro *chip, u16 reg, u16 val)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	__wp_write(chip, reg, val);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* =========================================================================
 * APU CRAM helpers  (BUG-1 fix)
 *
 * APU registers are in CRAM.  Access requires the two-step indirection:
 *   1. Write (apu<<4)|reg  →  WP[IDR1_CRAM_PTR]   (set CRAM address)
 *   2. Read/write data     ←→ WP[IDR0_DATA_PORT]   (CRAM data)
 *
 * We poll IDR1_CRAM_PTR after writing to confirm the pointer settled,
 * matching the behaviour of apu_index_set() in reference [1].
 *
 * ALL unlocked — callers must hold chip->reg_lock.
 * ======================================================================= */

static void apu_set_register(struct maestro *chip, u16 apu, u8 reg, u16 data)
{
	u16 idx = (u16)((apu << 4) | reg);
	int i;

	__wp_write(chip, IDR1_CRAM_PTR, idx);
	for (i = 0; i < 1000; i++)
		if (__wp_read(chip, IDR1_CRAM_PTR) == idx)
			break;
	__wp_write(chip, IDR0_DATA_PORT, data);
}

static u16 apu_get_register(struct maestro *chip, u16 apu, u8 reg)
{
	__wp_write(chip, IDR1_CRAM_PTR, (u16)((apu << 4) | reg));
	return __wp_read(chip, IDR0_DATA_PORT);
}

/* =========================================================================
 * WaveCache helpers  (BUG-2 fix)
 *
 * WaveCache channel registers: direct I/O at BAR0+0x10 / BAR0+0x12.
 * NOT through the WP index/data indirection.
 * Callers must hold chip->reg_lock.
 * ======================================================================= */

static inline void wc_write(struct maestro *chip, u16 reg, u16 val)
{
	outw(reg, chip->io_base + WC_INDEX);
	outw(val, chip->io_base + WC_DATA);
}

static inline u16 wc_read(struct maestro *chip, u16 reg)
{
	outw(reg, chip->io_base + WC_INDEX);
	return inw(chip->io_base + WC_DATA);
}

/* =========================================================================
 * WaveCache base-register programming  (BUG-7 fix)
 *
 * WC[0x01FC-0x01FF] must hold the page-frame number (addr>>12) of the
 * DMA pool.  Without this the WaveCache cannot locate system RAM.
 * Called from init and re-called on open() after power-up.
 * Callers must hold chip->reg_lock.
 * ======================================================================= */

static void maestro_set_wc_base(struct maestro *chip)
{
	u16 pfn = (u16)(chip->dma_addr >> 12);

	wc_write(chip, 0x01FC, pfn);
	wc_write(chip, 0x01FD, pfn);
	wc_write(chip, 0x01FE, pfn);
	wc_write(chip, 0x01FF, pfn);
}

/* =========================================================================
 * APU allocation
 * ======================================================================= */

/** Allocate a stereo pair; returns first APU index or -EBUSY. */
static int apu_alloc_pair(struct maestro *chip)
{
	int i;

	for (i = 0; i < NR_APUS; i += 2) {
		if (!chip->apu[i] && !chip->apu[i + 1]) {
			chip->apu[i] = chip->apu[i + 1] = 1;
			return i;
		}
	}
	return -EBUSY;
}

static void apu_free_pair(struct maestro *chip, int base)
{
	if (base < 0 || base + 1 >= NR_APUS)
		return;
	chip->apu[base] = chip->apu[base + 1] = 0;
}

/**
 * Allocate 4 consecutive APUs for capture.
 *
 * The capture chain requires apu[2] = apu[0]+2 (InputMixer reads from
 * ADC parallel port 0x14+offset and writes to mixbuf; SRC APU routes via
 * register 11 to the InputMixer APU at its own index + 2).
 */
static int apu_alloc_quad(struct maestro *chip)
{
	int i;

	for (i = 0; i <= NR_APUS - 4; i += 2) {
		if (!chip->apu[i]   && !chip->apu[i+1] &&
		    !chip->apu[i+2] && !chip->apu[i+3]) {
			chip->apu[i] = chip->apu[i+1] = 1;
			chip->apu[i+2] = chip->apu[i+3] = 1;
			return i;
		}
	}
	return -EBUSY;
}

static void apu_free_quad(struct maestro *chip, int base)
{
	int i;

	if (base < 0 || base + 3 >= NR_APUS)
		return;
	for (i = 0; i < 4; i++)
		chip->apu[base + i] = 0;
}

/* =========================================================================
 * AC97 codec access  (BUG-9 fix: no spinlock around delays)
 *
 * The AC97 layer serialises all calls with its own internal mutex.
 * Holding chip->reg_lock here would deadlock because msleep() (or even
 * the long udelay loop) cannot run inside a spinlock.
 * ======================================================================= */

static int ac97_wait_ready(struct maestro *chip)
{
	int t = 1000;

	while (t-- > 0) {
		if (!(inb(chip->io_base + ESM_AC97_INDEX) & 1))
			return 0;
		udelay(50);
	}
	dev_warn(&chip->pci->dev, "AC97 bus ready timeout\n");
	return -ETIMEDOUT;
}

static void maestro_ac97_write(struct snd_ac97 *ac97,
				unsigned short reg, unsigned short val)
{
	struct maestro *chip = ac97->private_data;

	ac97_wait_ready(chip);
	outw(val, chip->io_base + ESM_AC97_DATA);
	outb(reg,  chip->io_base + ESM_AC97_INDEX);
	udelay(50);
}

static unsigned short maestro_ac97_read(struct snd_ac97 *ac97,
					 unsigned short reg)
{
	struct maestro *chip = ac97->private_data;
	u16 data = 0;

	ac97_wait_ready(chip);
	outb(reg | 0x80, chip->io_base + ESM_AC97_INDEX);
	udelay(50);

	if (!ac97_wait_ready(chip))
		data = inw(chip->io_base + ESM_AC97_DATA);

	return data;
}

static const struct snd_ac97_bus_ops maestro_ac97_ops = {
	.write = maestro_ac97_write,
	.read  = maestro_ac97_read,
};

/* =========================================================================
 * DMA pool management
 * ======================================================================= */

static int maestro_init_dmabuf(struct maestro *chip, int kbytes)
{
	struct maestro_chunk *chunk;
	size_t sz = (size_t)kbytes * 1024;

	chip->dma_area = dma_alloc_coherent(&chip->pci->dev, sz,
					    &chip->dma_addr, GFP_KERNEL);
	if (!chip->dma_area) {
		dev_err(&chip->pci->dev,
			"Cannot allocate %d KiB DMA buffer\n", kbytes);
		return -ENOMEM;
	}
	chip->dma_bytes = sz;

	/* Verify 28-bit WaveCache addressing constraint */
	if ((chip->dma_addr + chip->dma_bytes - 1) >> 28) {
		dev_err(&chip->pci->dev,
			"DMA pool exceeds 28-bit WaveCache limit\n");
		dma_free_coherent(&chip->pci->dev, sz,
				  chip->dma_area, chip->dma_addr);
		chip->dma_area = NULL;
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&chip->buf_list);

	/* Zero the hardware-reserved head block (status FIFO) */
	memset(chip->dma_area, 0, DMA_POOL_RSVD);

	/* Create one large free chunk covering the usable region */
	chunk = kzalloc(sizeof(*chunk), GFP_KERNEL);
	if (!chunk) {
		dma_free_coherent(&chip->pci->dev, sz,
				  chip->dma_area, chip->dma_addr);
		chip->dma_area = NULL;
		return -ENOMEM;
	}

	chunk->buf   = (char *)chip->dma_area + DMA_POOL_RSVD;
	chunk->addr  = chip->dma_addr         + DMA_POOL_RSVD;
	chunk->size  = chip->dma_bytes        - DMA_POOL_RSVD;
	chunk->empty = 1;
	list_add_tail(&chunk->list, &chip->buf_list);

	dev_info(&chip->pci->dev, "DMA pool: %d KiB @ 0x%llx\n",
		 kbytes, (unsigned long long)chip->dma_addr);
	return 0;
}

static struct maestro_chunk *dma_alloc(struct maestro *chip, int size)
{
	struct maestro_chunk *c;

	if (size <= 0)
		return NULL;

	size = ALIGN(size, WC_ALIGN);

	list_for_each_entry(c, &chip->buf_list, list) {
		if (!c->empty || c->size < size)
			continue;
		if (c->size > size) {
			struct maestro_chunk *tail;

			tail = kzalloc(sizeof(*tail), GFP_ATOMIC);
			if (!tail)
				return NULL;
			tail->buf   = c->buf  + size;
			tail->addr  = c->addr + size;
			tail->size  = c->size - size;
			tail->empty = 1;
			c->size = size;
			list_add(&tail->list, &c->list);
		}
		c->empty = 0;
		return c;
	}
	dev_err(&chip->pci->dev, "DMA pool: no block >= %d bytes\n", size);
	return NULL;
}

static void dma_free_chunk(struct maestro *chip, struct maestro_chunk *c)
{
	struct maestro_chunk *nb;

	if (!c)
		return;

	c->empty = 1;

	if (!list_is_last(&c->list, &chip->buf_list)) {
		nb = list_next_entry(c, list);
		if (nb->empty) {
			c->size += nb->size;
			list_del(&nb->list);
			kfree(nb);
		}
	}
	if (c->list.prev != &chip->buf_list) {
		nb = list_prev_entry(c, list);
		if (nb->empty) {
			nb->size += c->size;
			list_del(&c->list);
			kfree(c);
		}
	}
}

static void maestro_free_dmabuf(struct maestro *chip)
{
	struct maestro_chunk *c, *tmp;

	if (!chip->dma_area)
		return;

	list_for_each_entry_safe(c, tmp, &chip->buf_list, list) {
		list_del(&c->list);
		kfree(c);
	}
	dma_free_coherent(&chip->pci->dev, chip->dma_bytes,
			  chip->dma_area, chip->dma_addr);
	chip->dma_area = NULL;
}

/* =========================================================================
 * Bob timer  (BUG-10 fix: start/stop acquire reg_lock internally)
 *
 * Lock ordering: substream_lock → reg_lock (never reversed).
 * maestro_bob_inc/dec must be called with substream_lock held.
 * maestro_bob_start/stop acquire reg_lock themselves.
 * ======================================================================= */

static void maestro_bob_stop(struct maestro *chip)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	__wp_write(chip, 0x11, __wp_read(chip, 0x11) & ~0x0001U);
	__wp_write(chip, 0x17, __wp_read(chip, 0x17) & ~0x0001U);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static void maestro_bob_start(struct maestro *chip)
{
	unsigned long flags;
	int prescale, divide;

	/* Compute dividers from chip->bob_freq */
	for (prescale = 5; prescale < 12; prescale++)
		if (chip->bob_freq > (int)(ESS_SYSCLK >> (prescale + 9)))
			break;

	divide = 1;
	while (prescale > 5 && divide < 32) { prescale--; divide <<= 1; }
	divide >>= 1;
	for (; divide < 31; divide++)
		if (chip->bob_freq >
		    (int)((ESS_SYSCLK >> (prescale + 9)) / (divide + 1)))
			break;
	if (divide == 0) {
		divide++;
		if (prescale > 5) prescale--;
	} else if (divide > 1) {
		divide--;
	}

	spin_lock_irqsave(&chip->reg_lock, flags);
	__wp_write(chip, IDR6_TIMER_CTRL, 0x9000 | (prescale << 5) | divide);
	__wp_write(chip, 0x11, __wp_read(chip, 0x11) | 1);
	__wp_write(chip, 0x17, __wp_read(chip, 0x17) | 1);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* Must be called with chip->substream_lock held */
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

/* Must be called with chip->substream_lock held */
static void maestro_bob_dec(struct maestro *chip)
{
	int clients = atomic_dec_return(&chip->bobclient);

	if (clients <= 0) {
		maestro_bob_stop(chip);
		atomic_set(&chip->bobclient, 0);
		return;
	}
	if (chip->bob_freq > MAESTRO_BOB_FREQ) {
		struct maestro_channel *ch;
		int max = MAESTRO_BOB_FREQ;

		list_for_each_entry(ch, &chip->substream_list, list)
			if (ch->bob_freq > max)
				max = ch->bob_freq;
		if (max != chip->bob_freq) {
			maestro_bob_stop(chip);
			chip->bob_freq = max;
			maestro_bob_start(chip);
		}
	}
}

static int calc_bob_freq(struct maestro *chip, struct maestro_channel *ch,
			  struct snd_pcm_runtime *rt)
{
	int freq = rt->rate * 4;

	if (ch->fmt & ESS_FMT_STEREO) freq <<= 1;
	if (ch->fmt & ESS_FMT_16BIT)  freq <<= 1;
	if (ch->frag_size)             freq /= ch->frag_size;
	return clamp(freq, MAESTRO_BOB_FREQ, MAESTRO_BOB_FREQ_MAX);
}

/* =========================================================================
 * APU programming helpers
 * ======================================================================= */

/**
 * maestro_compute_rate() - convert Hz to APU frequency register value
 *
 * BUG-3+4 fix:
 *  - chip->clock = 48828 (was 48000)
 *  - 48 kHz returns exactly 0x10000 (no rounding)
 *  - Full fixed-point: (f/clk)<<16 + ((f%clk)<<16)/clk
 *
 * Caller must hold chip->reg_lock.
 */
static u32 maestro_compute_rate(struct maestro *chip, u32 freq)
{
	u32 clk = chip->clock;

	if (freq == 48000)
		return 0x10000;
	return ((freq / clk) << 16) + (((freq % clk) << 16) / clk);
}

/* Caller must hold chip->reg_lock */
static void apu_set_freq(struct maestro *chip, int apu, u32 freq)
{
	apu_set_register(chip, apu, 2,
			 (apu_get_register(chip, apu, 2) & 0x00FF) |
			 ((freq & 0xFF) << 8) | 0x10);
	apu_set_register(chip, apu, 3, freq >> 8);
}

/*
 * Trigger / untrigger an APU.
 * The mode nibble goes into bits [7:4] of APU reg 0.
 * APU mode encoding (from reference [1] set_apu_fmt()):
 *   base=0x1, +0x2 if 8-bit, +0x1 if stereo
 *   → stored unshifted; this function shifts <<4 into reg 0.
 * Caller must hold chip->reg_lock.
 */
static void apu_trigger(struct maestro *chip, int apu, int mode)
{
	u16 v = apu_get_register(chip, apu, 0);

	v = (v & 0xFF0F) | ((mode & 0xF) << 4);
	apu_set_register(chip, apu, 0, v);
}

/* Caller must hold chip->reg_lock */
static void program_wc_channel(struct maestro *chip,
				struct maestro_channel *ch,
				int slot, u32 addr, int capture)
{
	u32 wc_val = (addr - 0x10) & 0xFFF8;

	if (!capture) {
		if (!(ch->fmt & ESS_FMT_16BIT)) wc_val |= 4;
		if (ch->fmt & ESS_FMT_STEREO)   wc_val |= 2;
	}
	wc_write(chip, ch->apu[slot] << 3, wc_val);
}

/* Read hardware DMA word-offset; caller must hold chip->reg_lock */
static unsigned int get_dma_ptr(struct maestro *chip,
				 struct maestro_channel *ch)
{
	return (apu_get_register(chip, ch->apu[0], 5) - ch->base[0]) & 0xFFFEU;
}

/* Arm all APUs; acquires reg_lock internally */
static void stream_start(struct maestro *chip, struct maestro_channel *ch)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);

	apu_set_register(chip, ch->apu[0], 5, ch->base[0]);
	apu_trigger(chip, ch->apu[0], ch->apu_mode[0]);

	if (ch->fmt & ESS_FMT_STEREO) {
		apu_set_register(chip, ch->apu[1], 5, ch->base[1]);
		apu_trigger(chip, ch->apu[1], ch->apu_mode[1]);
	}

	if (ch->mode == MAESTRO_MODE_CAPTURE) {
		/* Start InputMixer APUs first (upstream of SRC) */
		apu_set_register(chip, ch->apu[2], 5, ch->base[2]);
		apu_trigger(chip, ch->apu[2], ch->apu_mode[2]);
		apu_set_register(chip, ch->apu[3], 5, ch->base[3]);
		apu_trigger(chip, ch->apu[3], ch->apu_mode[3]);
	}

	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* Disarm all APUs; acquires reg_lock internally */
static void stream_stop(struct maestro *chip, struct maestro_channel *ch)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	apu_trigger(chip, ch->apu[0], 0);
	apu_trigger(chip, ch->apu[1], 0);
	if (ch->mode == MAESTRO_MODE_CAPTURE) {
		apu_trigger(chip, ch->apu[2], 0);
		apu_trigger(chip, ch->apu[3], 0);
	}
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* =========================================================================
 * Playback APU setup
 * ======================================================================= */

static void setup_playback(struct maestro *chip, struct maestro_channel *ch,
			    struct snd_pcm_runtime *rt)
{
	unsigned long flags;
	int high = 0, slot, i;
	u32 pa, freq;

	ch->dma_size  = snd_pcm_lib_buffer_bytes(ch->substream);
	ch->frag_size = snd_pcm_lib_period_bytes(ch->substream);
	ch->wav_shift = 1;   /* base shift: word (2 bytes) → 1 */
	ch->fmt       = 0;

	if (snd_pcm_format_width(rt->format) == 16)
		ch->fmt |= ESS_FMT_16BIT;
	if (rt->channels > 1) {
		ch->fmt |= ESS_FMT_STEREO;
		if (ch->fmt & ESS_FMT_16BIT)
			ch->wav_shift++;   /* stereo 16-bit: 2 shifts */
	}

	ch->bob_freq = calc_bob_freq(chip, ch, rt);

	if (ch->fmt & ESS_FMT_STEREO)
		high = 1;

	spin_lock_irqsave(&chip->reg_lock, flags);

	for (slot = 0; slot <= high; slot++) {
		int apu = ch->apu[slot];
		int size = ch->dma_size >> ch->wav_shift;

		program_wc_channel(chip, ch, slot, ch->memory->addr, 0);

		pa  = (u32)ch->memory->addr - (u32)chip->dma_addr;
		pa >>= 1;           /* byte → word                    */
		pa |= 0x00400000;   /* system RAM flag                */

		if (ch->fmt & ESS_FMT_STEREO) {
			if (slot) pa |= 0x00800000;  /* right channel   */
			if (ch->fmt & ESS_FMT_16BIT) pa >>= 1;
		}

		ch->base[slot] = pa & 0xFFFF;

		for (i = 0; i < NR_APU_REGS; i++)
			apu_set_register(chip, apu, i, 0x0000);

		apu_set_register(chip, apu,  4, ((pa >> 16) & 0xFF) << 8);
		apu_set_register(chip, apu,  5, pa & 0xFFFF);
		apu_set_register(chip, apu,  6, (pa + size) & 0xFFFF);
		apu_set_register(chip, apu,  7, size);
		apu_set_register(chip, apu,  8, 0x0000);
		apu_set_register(chip, apu,  9, 0xD000);  /* amplitude  */
		apu_set_register(chip, apu, 11, 0x0000);
		apu_set_register(chip, apu,  0, 0x400F);  /* DMA on, no env */

		/* APU mode nibble (unshifted; apu_trigger() shifts <<4) */
		ch->apu_mode[slot] = (ch->fmt & ESS_FMT_16BIT) ? 0x1 : 0x3;
		if (ch->fmt & ESS_FMT_STEREO)
			ch->apu_mode[slot]++;

		if (ch->fmt & ESS_FMT_STEREO)
			apu_set_register(chip, apu, 10,
					 0x8F00 | (slot ? 0 : 0x10));
		else
			apu_set_register(chip, apu, 10, 0x8F08);
	}

	/* Enable WP + DirectSound interrupts */
	outw(1, chip->io_base + 0x04);
	outw(inw(chip->io_base + ESM_HOST_IRQ) | ESM_HIRQ_DSIE,
	     chip->io_base + ESM_HOST_IRQ);

	spin_unlock_irqrestore(&chip->reg_lock, flags);

	/* Set sample rate (BUG-3+4) */
	{
		u32 r = clamp_val(rt->rate, 4000U, 48000U);

		if (!(ch->fmt & ESS_FMT_16BIT) && !(ch->fmt & ESS_FMT_STEREO))
			r >>= 1;   /* 8-bit mono: half-rate */

		freq = maestro_compute_rate(chip, r);
	}

	spin_lock_irqsave(&chip->reg_lock, flags);
	apu_set_freq(chip, ch->apu[0], freq);
	apu_set_freq(chip, ch->apu[1], freq);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* =========================================================================
 * Capture APU setup  (BUG-11 fix)
 *
 * APU assignment:
 *   ch->apu[0] = SRC-L   (Sample Rate Converter, mode 0xB)
 *   ch->apu[1] = SRC-R
 *   ch->apu[2] = InMix-L (Input Mixer from ADC parallel bus, mode 0x9)
 *   ch->apu[3] = InMix-R
 *
 * Data flow (reference [1] ess_rec_setup()):
 *   ADC → InMix APU → mixbuf (intermediate 512B) → SRC APU → DMA buffer
 *
 * InMix APUs run at fixed 48 kHz (codec native rate, freq=0x10000).
 * SRC APUs run at the requested capture rate.
 * SRC APU reg[11] = route = InMix APU index (= SRC index + 2).
 * InMix APU reg[11] = 0x14 + channel (ADC parallel input port).
 * ======================================================================= */

static void setup_capture(struct maestro *chip, struct maestro_channel *ch,
			   struct snd_pcm_runtime *rt)
{
	unsigned long flags;
	int i;
	u32 freq;

	ch->dma_size  = snd_pcm_lib_buffer_bytes(ch->substream);
	ch->frag_size = snd_pcm_lib_period_bytes(ch->substream);
	/* Capture locked to stereo 16-bit */
	ch->fmt       = ESS_FMT_16BIT | ESS_FMT_STEREO;
	ch->wav_shift = 2;
	ch->bob_freq  = calc_bob_freq(chip, ch, rt);

	spin_lock_irqsave(&chip->reg_lock, flags);

	/* ── SRC APUs ────────────────────────────────────────────────── */
	for (i = 0; i < 2; i++) {
		int apu  = ch->apu[i];
		int size = (ch->dma_size >> ch->wav_shift);
		u32 pa;
		int j;

		wc_write(chip, apu << 3,
			 ((ch->memory->addr - 0x10) & 0xFFF8) | 2);

		pa  = (u32)ch->memory->addr - (u32)chip->dma_addr;
		pa >>= 1;
		pa |= 0x00400000;
		if (i) pa |= 0x00800000;   /* right channel */
		pa >>= 1;                   /* stereo 16-bit */

		ch->base[i] = pa & 0xFFFF;

		for (j = 0; j < NR_APU_REGS; j++)
			apu_set_register(chip, apu, j, 0x0000);

		apu_set_register(chip, apu,  0, 0x400F);
		apu_set_register(chip, apu,  2, 0x0008);  /* subgroup enable */
		apu_set_register(chip, apu,  4, ((pa >> 16) & 0xFF) << 8);
		apu_set_register(chip, apu,  5, pa & 0xFFFF);
		apu_set_register(chip, apu,  6, (pa + size) & 0xFFFF);
		apu_set_register(chip, apu,  7, size);
		apu_set_register(chip, apu,  8, 0x00F0);
		apu_set_register(chip, apu,  9, 0x0000);
		apu_set_register(chip, apu, 10, 0x8F08);
		/* Route: SRC reads from InMix at apu[i+2] */
		apu_set_register(chip, apu, 11, ch->apu[i + 2]);

		ch->apu_mode[i] = 0xB;  /* SRC mode */
	}

	/* ── InputMixer APUs ─────────────────────────────────────────── */
	for (i = 0; i < 2; i++) {
		int apu      = ch->apu[2 + i];
		int mixsize  = (MIXBUF_SIZE / 2) >> 1;  /* words per channel */
		u32 mixpa;
		int j;

		wc_write(chip, apu << 3,
			 ((ch->mixbuf->addr - 0x10) & 0xFFF8) | 2);

		mixpa  = (u32)ch->mixbuf->addr + (u32)(i * MIXBUF_SIZE / 2);
		mixpa -= (u32)chip->dma_addr;
		mixpa >>= 1;
		mixpa |= 0x00400000;

		ch->base[2 + i] = mixpa & 0xFFFF;

		for (j = 0; j < NR_APU_REGS; j++)
			apu_set_register(chip, apu, j, 0x0000);

		apu_set_register(chip, apu,  0, 0x400F);
		apu_set_register(chip, apu,  2, 0x0008);
		apu_set_register(chip, apu,  4, ((mixpa >> 16) & 0xFF) << 8);
		apu_set_register(chip, apu,  5, mixpa & 0xFFFF);
		apu_set_register(chip, apu,  6, (mixpa + mixsize) & 0xFFFF);
		apu_set_register(chip, apu,  7, mixsize);
		apu_set_register(chip, apu,  8, 0x00F0);
		apu_set_register(chip, apu,  9, 0x0000);
		apu_set_register(chip, apu, 10, 0x8F08);
		/* Route: InMix reads from ADC parallel port 0x14/0x15 */
		apu_set_register(chip, apu, 11, 0x14 + i);

		ch->apu_mode[2 + i] = 0x9;  /* InputMixer mode */

		/* InMix always at 48 kHz (codec native) */
		apu_set_freq(chip, apu, 0x10000);
	}

	outw(1, chip->io_base + 0x04);
	outw(inw(chip->io_base + ESM_HOST_IRQ) | ESM_HIRQ_DSIE,
	     chip->io_base + ESM_HOST_IRQ);

	spin_unlock_irqrestore(&chip->reg_lock, flags);

	/* SRC rate: clamp to 47999 (0x10000 is reserved for 48 kHz fixed) */
	freq = maestro_compute_rate(chip,
		clamp_val(rt->rate, 4000U, 47999U));

	spin_lock_irqsave(&chip->reg_lock, flags);
	apu_set_freq(chip, ch->apu[0], freq);
	apu_set_freq(chip, ch->apu[1], freq);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* =========================================================================
 * PCM operations
 * ======================================================================= */

static const struct snd_pcm_hardware maestro_hw_play = {
	.info         = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
	                SNDRV_PCM_INFO_INTERLEAVED |
	                SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_RESUME,
	.formats      = SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE,
	.rates        = SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min     = 4000,
	.rate_max     = 48000,
	.channels_min = 1,
	.channels_max = 2,
	.period_bytes_min = 256,
	.periods_min  = 1,
	.periods_max  = 1024,
};

static const struct snd_pcm_hardware maestro_hw_cap = {
	.info         = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
	                SNDRV_PCM_INFO_INTERLEAVED |
	                SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_RESUME,
	.formats      = SNDRV_PCM_FMTBIT_S16_LE,  /* capture: 16-bit only */
	.rates        = SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min     = 4000,
	.rate_max     = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.period_bytes_min = 256,
	.periods_min  = 1,
	.periods_max  = 1024,
};

/* ── Playback open/close ─────────────────────────────────────────────── */

static int maestro_play_open(struct snd_pcm_substream *sub)
{
	struct maestro *chip = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *rt = sub->runtime;
	struct maestro_channel *ch;
	int apu;

	apu = apu_alloc_pair(chip);
	if (apu < 0)
		return apu;

	ch = kzalloc(sizeof(*ch), GFP_KERNEL);
	if (!ch) {
		apu_free_pair(chip, apu);
		return -ENOMEM;
	}

	ch->chip      = chip;
	ch->substream = sub;
	ch->apu[0]    = apu;
	ch->apu[1]    = apu + 1;
	ch->mode      = MAESTRO_MODE_PLAY;
	spin_lock_init(&ch->lock);
	INIT_LIST_HEAD(&ch->list);

	rt->private_data = ch;
	rt->hw = maestro_hw_play;
	rt->hw.buffer_bytes_max = chip->dma_bytes - DMA_POOL_RSVD;
	rt->hw.period_bytes_max = chip->dma_bytes - DMA_POOL_RSVD;

	spin_lock_irq(&chip->substream_lock);
	list_add(&ch->list, &chip->substream_list);
	spin_unlock_irq(&chip->substream_lock);
	return 0;
}

static int maestro_play_close(struct snd_pcm_substream *sub)
{
	struct maestro *chip = snd_pcm_substream_chip(sub);
	struct maestro_channel *ch = sub->runtime->private_data;

	if (!ch) return 0;
	spin_lock_irq(&chip->substream_lock);
	list_del(&ch->list);
	spin_unlock_irq(&chip->substream_lock);
	apu_free_pair(chip, ch->apu[0]);
	kfree(ch);
	sub->runtime->private_data = NULL;
	return 0;
}

/* ── Capture open/close ──────────────────────────────────────────────── */

static int maestro_cap_open(struct snd_pcm_substream *sub)
{
	struct maestro *chip = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *rt = sub->runtime;
	struct maestro_channel *ch;
	int apu;

	apu = apu_alloc_quad(chip);
	if (apu < 0)
		return apu;

	ch = kzalloc(sizeof(*ch), GFP_KERNEL);
	if (!ch) {
		apu_free_quad(chip, apu);
		return -ENOMEM;
	}

	ch->chip      = chip;
	ch->substream = sub;
	ch->apu[0]    = apu;
	ch->apu[1]    = apu + 1;
	ch->apu[2]    = apu + 2;
	ch->apu[3]    = apu + 3;
	ch->mode      = MAESTRO_MODE_CAPTURE;
	spin_lock_init(&ch->lock);
	INIT_LIST_HEAD(&ch->list);

	rt->private_data = ch;
	rt->hw = maestro_hw_cap;
	rt->hw.buffer_bytes_max = chip->dma_bytes - DMA_POOL_RSVD;
	rt->hw.period_bytes_max = chip->dma_bytes - DMA_POOL_RSVD;

	spin_lock_irq(&chip->substream_lock);
	list_add(&ch->list, &chip->substream_list);
	spin_unlock_irq(&chip->substream_lock);
	return 0;
}

static int maestro_cap_close(struct snd_pcm_substream *sub)
{
	struct maestro *chip = snd_pcm_substream_chip(sub);
	struct maestro_channel *ch = sub->runtime->private_data;

	if (!ch) return 0;
	spin_lock_irq(&chip->substream_lock);
	list_del(&ch->list);
	spin_unlock_irq(&chip->substream_lock);
	apu_free_quad(chip, ch->apu[0]);
	kfree(ch);
	sub->runtime->private_data = NULL;
	return 0;
}

/* ── hw_params / hw_free ─────────────────────────────────────────────── */

static int maestro_hw_params(struct snd_pcm_substream *sub,
			      struct snd_pcm_hw_params *params)
{
	struct maestro *chip = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *rt = sub->runtime;
	struct maestro_channel *ch = rt->private_data;
	int sz = params_buffer_bytes(params);

	/* Reuse existing buffer if large enough */
	if (ch->memory) {
		if (ch->memory->size >= sz) {
			rt->dma_area  = ch->memory->buf;
			rt->dma_addr  = ch->memory->addr;
			rt->dma_bytes = sz;
			goto mixbuf;
		}
		dma_free_chunk(chip, ch->memory);
		ch->memory = NULL;
	}

	ch->memory = dma_alloc(chip, sz);
	if (!ch->memory)
		return -ENOMEM;

	rt->dma_area  = ch->memory->buf;
	rt->dma_addr  = ch->memory->addr;
	rt->dma_bytes = sz;

mixbuf:
	if (ch->mode == MAESTRO_MODE_CAPTURE && !ch->mixbuf) {
		ch->mixbuf = dma_alloc(chip, MIXBUF_SIZE);
		if (!ch->mixbuf) {
			dma_free_chunk(chip, ch->memory);
			ch->memory = NULL;
			return -ENOMEM;
		}
		memset(ch->mixbuf->buf, 0, MIXBUF_SIZE);
	}
	return 0;
}

static int maestro_hw_free(struct snd_pcm_substream *sub)
{
	struct maestro *chip = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *rt = sub->runtime;
	struct maestro_channel *ch = rt->private_data;

	if (!ch) return 0;
	if (ch->mixbuf)  { dma_free_chunk(chip, ch->mixbuf);  ch->mixbuf  = NULL; }
	if (ch->memory)  { dma_free_chunk(chip, ch->memory);  ch->memory  = NULL; }
	rt->dma_area  = NULL;
	rt->dma_addr  = 0;
	rt->dma_bytes = 0;
	return 0;
}

/* ── prepare / trigger / pointer ─────────────────────────────────────── */

static int maestro_prepare(struct snd_pcm_substream *sub)
{
	struct maestro *chip = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *rt = sub->runtime;
	struct maestro_channel *ch = rt->private_data;

	if (!ch || !ch->memory)
		return -EINVAL;

	ch->hwptr = 0;
	ch->count = 0;

	if (ch->mode == MAESTRO_MODE_PLAY)
		setup_playback(chip, ch, rt);
	else
		setup_capture(chip, ch, rt);
	return 0;
}

static int maestro_trigger(struct snd_pcm_substream *sub, int cmd)
{
	struct maestro *chip = snd_pcm_substream_chip(sub);
	struct maestro_channel *ch = sub->runtime->private_data;
	int ret = 0;

	spin_lock(&chip->substream_lock);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!ch->running) {
			maestro_bob_inc(chip, ch->bob_freq);
			ch->hwptr = ch->count = 0;
			stream_start(chip, ch);
			ch->running = 1;
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (ch->running) {
			stream_stop(chip, ch);
			ch->running = 0;
			maestro_bob_dec(chip);
		}
		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock(&chip->substream_lock);
	return ret;
}

static snd_pcm_uframes_t maestro_pointer(struct snd_pcm_substream *sub)
{
	struct maestro *chip = snd_pcm_substream_chip(sub);
	struct maestro_channel *ch = sub->runtime->private_data;
	unsigned long flags;
	unsigned int ptr;

	if (!ch || !ch->memory)
		return 0;

	spin_lock_irqsave(&chip->reg_lock, flags);
	ptr = get_dma_ptr(chip, ch);
	spin_unlock_irqrestore(&chip->reg_lock, flags);

	return bytes_to_frames(sub->runtime,
			       (ptr << ch->wav_shift) % ch->dma_size);
}

static const struct snd_pcm_ops maestro_play_ops = {
	.open      = maestro_play_open,
	.close     = maestro_play_close,
	.hw_params = maestro_hw_params,
	.hw_free   = maestro_hw_free,
	.prepare   = maestro_prepare,
	.trigger   = maestro_trigger,
	.pointer   = maestro_pointer,
};

static const struct snd_pcm_ops maestro_cap_ops = {
	.open      = maestro_cap_open,
	.close     = maestro_cap_close,
	.hw_params = maestro_hw_params,
	.hw_free   = maestro_hw_free,
	.prepare   = maestro_prepare,
	.trigger   = maestro_trigger,
	.pointer   = maestro_pointer,
};

/* =========================================================================
 * Interrupt handler
 * ======================================================================= */

static void update_stream(struct maestro *chip, struct maestro_channel *ch)
{
	unsigned long flags;
	unsigned int hwptr, diff;

	if (!ch->substream || !ch->running)
		return;

	spin_lock_irqsave(&chip->reg_lock, flags);
	hwptr = get_dma_ptr(chip, ch);
	spin_unlock_irqrestore(&chip->reg_lock, flags);

	hwptr  = (hwptr << ch->wav_shift) % ch->dma_size;
	diff   = (ch->dma_size + hwptr - ch->hwptr) % ch->dma_size;
	ch->hwptr  = hwptr;
	ch->count += diff;

	if (ch->count >= ch->frag_size) {
		ch->count %= ch->frag_size;
		spin_unlock(&chip->substream_lock);
		snd_pcm_period_elapsed(ch->substream);
		spin_lock(&chip->substream_lock);
	}
}

static irqreturn_t maestro_interrupt(int irq, void *dev_id)
{
	struct maestro *chip = dev_id;
	struct maestro_channel *ch;
	u8 event;

	event = inb(chip->io_base + ESM_IRQ_STATUS);
	if (!event)
		return IRQ_NONE;

	outw(inw(chip->io_base + 0x04) & 1, chip->io_base + 0x04);
	outb(0xFF, chip->io_base + ESM_IRQ_STATUS);

	if (event & ESM_SOUND_IRQ) {
		spin_lock(&chip->substream_lock);
		list_for_each_entry(ch, &chip->substream_list, list)
			update_stream(chip, ch);
		spin_unlock(&chip->substream_lock);
	}

	if (event & ESM_HWVOL_IRQ)
		inw(chip->io_base + ESM_HOST_IRQ); /* ACK; TODO: vol control */

	return IRQ_HANDLED;
}

/* =========================================================================
 * Hardware initialisation  (BUG-5, BUG-6, BUG-7 fixes)
 *
 * Ported from maestro_config() in reference [1] with additions from
 * schematic [3] and binary analysis [2].
 * ======================================================================= */

/** Pulse bit 13 of port 0x18 to reset the WaveProcessor */
static void maestro_sound_reset(struct maestro *chip)
{
	outw(0x2000, chip->io_base + ESM_HOST_IRQ);
	udelay(1);
	outw(0x0000, chip->io_base + ESM_HOST_IRQ);
	udelay(1);
}

/**
 * maestro_ac97_reset() — BUG-6 fix
 *
 * Full cold-reset of both AC97 codecs via GPIO and ring-bus.
 * Ported from maestro_ac97_reset() in reference [1].
 * Schematic [3] page 1: codec reset lines go through GPIO[0] and GPIO[3].
 */
static void maestro_ac97_reset(struct maestro *chip)
{
	unsigned long io = chip->io_base;
	u16 save_68, w;

	outw(inw(io + 0x38) & 0xFFFC, io + 0x38);
	outw(inw(io + 0x3A) & 0xFFFC, io + 0x3A);
	outw(inw(io + 0x3C) & 0xFFFC, io + 0x3C);

	/* First codec reset */
	outw(0x0000, io + RING_BUS_CTRL_H);
	save_68 = inw(io + 0x68);
	pci_read_config_word(chip->pci, 0x58, &w);
	if (w & 0x1)
		save_68 |= 0x10;
	outw(0xFFFE, io + ESM_GPIO_MASK);
	outw(0x0001, io + 0x68);
	outw(0x0000, io + ESM_GPIO_DATA);
	udelay(20);
	outw(0x0001, io + ESM_GPIO_DATA);
	mdelay(20);

	outw(save_68 | 0x1,                io + 0x68);
	outw((inw(io + 0x38) & 0xFFFC)|1, io + 0x38);
	outw((inw(io + 0x3A) & 0xFFFC)|1, io + 0x3A);
	outw((inw(io + 0x3C) & 0xFFFC)|1, io + 0x3C);

	/* Second codec reset */
	outw(0x0000, io + RING_BUS_CTRL_H);
	outw(0xFFF7, io + ESM_GPIO_MASK);
	save_68 = inw(io + 0x68);
	outw(0x0009, io + 0x68);
	outw(0x0001, io + ESM_GPIO_DATA);
	udelay(20);
	outw(0x0009, io + ESM_GPIO_DATA);
	mdelay(500);

	outw(inw(io + 0x38) & 0xFFFC, io + 0x38);
	outw(inw(io + 0x3A) & 0xFFFC, io + 0x3A);
	outw(inw(io + 0x3C) & 0xFFFC, io + 0x3C);
}

/**
 * maestro_chip_init() — BUG-5 fix (complete from scratch)
 *
 * Performs the full initialisation sequence required before any audio
 * operation.  Closely follows maestro_config() in reference [1].
 */
static int maestro_chip_init(struct maestro *chip)
{
	unsigned long io = chip->io_base;
	unsigned long flags;
	u16 w;
	u32 n;
	int apu, reg;

	/* ── PCI config ───────────────────────────────────────────────── */
	pci_read_config_word(chip->pci, PCI_CFG_MAESTRO_A, &w);
	w &= ~BIT(5);          /* no L/R swap */
	pci_write_config_word(chip->pci, PCI_CFG_MAESTRO_A, w);

	pci_read_config_word(chip->pci, PCI_CFG_MAESTRO_B, &w);
	w &= ~BIT(15);         /* internal clock multiplier off */
	w &= ~BIT(14);         /* external clock                */
	w &= ~BIT(7);          /* HW volume off                 */
	w &= ~BIT(6);          /* debounce off                  */
	w &= ~BIT(5);          /* GPIO 4:5 normal               */
	w |=  BIT(4);          /* disconnect from CHI           */
	w &= ~BIT(2);          /* MIDI fix off                  */
	w &= ~BIT(1);          /* reserved                      */
	pci_write_config_word(chip->pci, PCI_CFG_MAESTRO_B, w);

	pci_read_config_word(chip->pci, PCI_CFG_LEGACY_CTL, &w);
	w |=  BIT(15);         /* legacy decode off             */
	w &= ~BIT(14);         /* SIRQ off                      */
	w &= ~0x1F;            /* disable SB/FM/game/MPU legacy */
	pci_write_config_word(chip->pci, PCI_CFG_LEGACY_CTL, w);

	/* ── Soft reset ───────────────────────────────────────────────── */
	maestro_sound_reset(chip);

	/* ── Ring bus initial setup ───────────────────────────────────── */
	outw(0xC090, io + RING_BUS_CTRL_L);  /* DirectSound stereo */
	udelay(20);
	outw(0x3000, io + RING_BUS_CTRL_H);
	udelay(20);

	/* ── AC97 codec cold reset (BUG-6) ───────────────────────────── */
	maestro_ac97_reset(chip);

	/* ── Ring bus full configuration ─────────────────────────────── */
	n  = inl(io+RING_BUS_CTRL_L); n &= ~0xF000; n |= 12 << 12; outl(n, io+RING_BUS_CTRL_L);
	n  = inl(io+RING_BUS_CTRL_L); n &= ~0x0F00;                outl(n, io+RING_BUS_CTRL_L);
	n  = inl(io+RING_BUS_CTRL_L); n &= ~0x00F0; n |= 9 << 4;   outl(n, io+RING_BUS_CTRL_L);
	n  = inl(io+RING_BUS_CTRL_L); n &= ~0x000F;                outl(n, io+RING_BUS_CTRL_L);
	n  = inl(io+RING_BUS_CTRL_L); n |=  BIT(29);               outl(n, io+RING_BUS_CTRL_L);
	n  = inl(io+RING_BUS_CTRL_L); n |=  BIT(28);               outl(n, io+RING_BUS_CTRL_L);
	n  = inl(io+RING_BUS_CTRL_L); n &= ~0x00F00000;            outl(n, io+RING_BUS_CTRL_L);
	n  = inl(io+RING_BUS_CTRL_L); n &= ~0x000F0000;            outl(n, io+RING_BUS_CTRL_L);

	/* ── Host IRQ setup ───────────────────────────────────────────── */
	w  = inw(io + ESM_HOST_IRQ);
	w &= ~BIT(7);   /* ClkRun off       */
	w &= ~BIT(6);   /* Harpo off        */
	w &= ~BIT(4);   /* ASSP IRQ off     */
	w &= ~BIT(3);   /* ISDN IRQ off     */
	w |=  BIT(2);   /* DirectSound on   */
	w &= ~BIT(1);   /* MPU401 off       */
	w |=  BIT(0);   /* SB IRQ on        */
	outw(w, io + ESM_HOST_IRQ);

	/* ── ASSP registers (required even when ASSP unused) ─────────── */
	outb(0, io + 0xA4);
	outb(3, io + 0xA2);
	outb(0, io + 0xA6);

	spin_lock_irqsave(&chip->reg_lock, flags);

	/* ── Clear WC buffer descriptor regions 0x01D0-0x01EF ────────── */
	for (apu = 0; apu < 16; apu++) {
		outw(0x01E0 + apu, io + WC_INDEX); outw(0, io + WC_DATA);
		outw(0x01D0 + apu, io + WC_INDEX); outw(0, io + WC_DATA);
	}

	/* ── IDR7: enable system RAM for WaveCache ────────────────────── */
	wc_write(chip, IDR7_WAVE_ROMRAM,
		 wc_read(chip, IDR7_WAVE_ROMRAM) & 0xFF00);
	wc_write(chip, IDR7_WAVE_ROMRAM,
		 wc_read(chip, IDR7_WAVE_ROMRAM) | 0x0100);
	wc_write(chip, IDR7_WAVE_ROMRAM,
		 wc_read(chip, IDR7_WAVE_ROMRAM) & ~0x0200);
	wc_write(chip, IDR7_WAVE_ROMRAM,
		 wc_read(chip, IDR7_WAVE_ROMRAM) | 0x0400);

	/* ── WP control registers (from reference [1]) ────────────────── */
	__wp_write(chip, IDR2_CRAM_DATA, 0x0000);
	__wp_write(chip, 0x08, 0xB004);
	__wp_write(chip, 0x09, 0x001B);
	__wp_write(chip, 0x0A, 0x8000);
	__wp_write(chip, 0x0B, 0x3F37);
	__wp_write(chip, 0x0C, 0x0098);
	/* Parallel output routing */
	__wp_write(chip, 0x0C, (__wp_read(chip, 0x0C) & ~0xF000) | 0x8000);
	/* Parallel input routing (recording path) */
	__wp_write(chip, 0x0C, (__wp_read(chip, 0x0C) & ~0x0F00) | 0x0500);
	__wp_write(chip, 0x0D, 0x7632);

	/* ── WaveCache control (size=2MB, enable) ─────────────────────── */
	outw(inw(io + WC_CONTROL) | BIT(8),  io + WC_CONTROL);
	outw(inw(io + WC_CONTROL) & 0xFE03,  io + WC_CONTROL);
	outw(inw(io + WC_CONTROL) & 0xFFFC,  io + WC_CONTROL);
	outw(inw(io + WC_CONTROL) | BIT(7),  io + WC_CONTROL);
	outw(0xA1A0,                          io + WC_CONTROL);

	/* ── Clear all 64 APUs ────────────────────────────────────────── */
	for (apu = 0; apu < NR_APUS; apu++)
		for (reg = 0; reg < NR_APU_REGS; reg++)
			apu_set_register(chip, apu, reg, 0);

	/* ── WaveCache base registers (BUG-7) ─────────────────────────── */
	maestro_set_wc_base(chip);

	spin_unlock_irqrestore(&chip->reg_lock, flags);

	/* BUG-3: correct clock reference */
	chip->clock = MAESTRO_CLOCK;

	dev_info(&chip->pci->dev, "Maestro-2E initialised (clock=%d Hz)\n",
		 chip->clock);
	return 0;
}

/* =========================================================================
 * AC97 mixer setup
 * ======================================================================= */

static int maestro_ac97_attach(struct maestro *chip)
{
	struct snd_ac97_bus *bus;
	struct snd_ac97_template ac97 = {};
	int err;

	/* Misc AC97 defaults from reference [1] maestro_ac97_init() */
	outw(0x0404, chip->io_base + ESM_AC97_DATA);
	outb(0x1E,   chip->io_base + ESM_AC97_INDEX);  /* Aux out */
	udelay(50);
	outw(0x0000, chip->io_base + ESM_AC97_DATA);
	outb(0x20,   chip->io_base + ESM_AC97_INDEX);  /* Misc = 0 */
	udelay(50);

	err = snd_ac97_bus(chip->card, 0, &maestro_ac97_ops, chip, &bus);
	if (err)
		return err;

	ac97.private_data = chip;
	ac97.pci          = chip->pci;
	ac97.scaps        = AC97_SCAP_AUDIO;

	err = snd_ac97_mixer(bus, &ac97, &chip->ac97);
	if (err) {
		chip->ac97 = NULL;
		return err;
	}
	dev_info(&chip->pci->dev, "AC97 codec: 0x%08x\n", chip->ac97->id);
	return 0;
}

/* =========================================================================
 * PCI probe / remove
 * ======================================================================= */

static int maestro_probe(struct pci_dev *pdev,
			  const struct pci_device_id *id)
{
	struct maestro  *chip;
	struct snd_card *card;
	struct snd_pcm  *pcm;
	resource_size_t  bar_start, bar_len;
	int err;

	err = pci_enable_device(pdev);
	if (err) return err;
	pci_set_master(pdev);

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
	if (err) {
		dev_err(&pdev->dev, "28-bit DMA unavailable\n");
		goto e_disable;
	}

	err = snd_card_new(&pdev->dev, -1, "ISIS", THIS_MODULE, 0, &card);
	if (err) goto e_disable;

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip) { err = -ENOMEM; goto e_card; }

	chip->card = card;
	chip->pci  = pdev;
	card->private_data = chip;
	pci_set_drvdata(pdev, chip);

	spin_lock_init(&chip->reg_lock);
	spin_lock_init(&chip->substream_lock);
	INIT_LIST_HEAD(&chip->buf_list);
	INIT_LIST_HEAD(&chip->substream_list);
	atomic_set(&chip->bobclient, 0);
	chip->bob_freq = MAESTRO_BOB_FREQ;

	bar_start = pci_resource_start(pdev, 0);
	bar_len   = pci_resource_len(pdev, 0);

	if (!request_region(bar_start, bar_len, DRIVER_NAME)) {
		dev_err(&pdev->dev, "I/O region busy\n");
		err = -EBUSY; goto e_chip;
	}
	chip->io_base = bar_start;

	err = request_irq(pdev->irq, maestro_interrupt,
			  IRQF_SHARED, DRIVER_NAME, chip);
	if (err) {
		dev_err(&pdev->dev, "IRQ %d unavailable\n", pdev->irq);
		goto e_region;
	}
	chip->irq = pdev->irq;

	err = maestro_init_dmabuf(chip, 512);
	if (err) goto e_irq;

	err = maestro_chip_init(chip);
	if (err) goto e_dma;

	err = snd_pcm_new(card, "Maestro PCM", 0, 1, 1, &pcm);
	if (err) goto e_dma;

	chip->pcm = pcm;
	pcm->private_data = chip;
	strscpy(pcm->name, "Maxi Studio ISIS", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &maestro_play_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,  &maestro_cap_ops);

	err = maestro_ac97_attach(chip);
	if (err) goto e_dma;

	strscpy(card->driver,    DRIVER_NAME,          sizeof(card->driver));
	strscpy(card->shortname, "Guillemot Maxi Studio ISIS",
		sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "%s at 0x%lx IRQ %d", card->shortname,
		 chip->io_base, chip->irq);

	err = snd_card_register(card);
	if (err) goto e_dma;

	dev_info(&pdev->dev, "%s registered (v%s)\n",
		 card->shortname, DRIVER_VERSION);
	return 0;

e_dma:    maestro_free_dmabuf(chip);
e_irq:    free_irq(chip->irq, chip);
e_region: release_region(chip->io_base, bar_len);
e_chip:   kfree(chip);
e_card:   snd_card_free(card);
e_disable: pci_disable_device(pdev);
	return err;
}

static void maestro_remove(struct pci_dev *pdev)
{
	struct maestro *chip = pci_get_drvdata(pdev);

	if (!chip) return;
	outw(0, chip->io_base + ESM_HOST_IRQ);
	free_irq(chip->irq, chip);
	maestro_free_dmabuf(chip);
	snd_card_free(chip->card);
	release_region(chip->io_base, pci_resource_len(pdev, 0));
	pci_disable_device(pdev);
	kfree(chip);
	pci_set_drvdata(pdev, NULL);
}

static const struct pci_device_id maestro_ids[] = {
	{ PCI_DEVICE(MAESTRO_VENDOR, MAESTRO_DEVICE) },
	{ }
};
MODULE_DEVICE_TABLE(pci, maestro_ids);

static struct pci_driver maestro_driver = {
	.name     = DRIVER_NAME,
	.id_table = maestro_ids,
	.probe    = maestro_probe,
	.remove   = maestro_remove,
};

module_pci_driver(maestro_driver);

MODULE_AUTHOR("ISISALSA Project <https://github.com/ISISALSA>");
MODULE_DESCRIPTION("ALSA driver for Guillemot Maxi Studio ISIS — Maestro-2E audio engine");
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
