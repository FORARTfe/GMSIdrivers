// SPDX-License-Identifier: GPL-2.0-only
/*
 * maestro2em.c  —  ALSA driver for Guillemot Maxi Studio ISIS
 *
 * Chip: ESS Maestro-2E  (PCI ID 0x125D:0x1978)
 *
 * Merged, fixed and modernized for current Linux kernels from
 * three earlier source versions (see project history).
 *
 * Key features
 * ────────────
 *  • Private DMA pool  (28-bit addressing required by WaveCache)
 *  • ISIS / SAM co-processor firmware loading  (pci64.bin)
 *  • AC97 codec via ESM register bridge
 *  • PCM playback through the 64-APU WaveProcessor engine
 *  • Bob timer for period-elapsed interrupt generation
 *
 * Known limitations / assumptions
 * ────────────────────────────────
 *  • Capture path is stubbed (APU slots reserved but not programmed);
 *    hardware documentation for the capture routing is incomplete.
 *  • Hardware volume control ISR is noted but not implemented.
 *  • 8-bit mono/stereo wav_shift arithmetic follows the original
 *    reference driver; subtle rounding may exist for 8-bit formats.
 *  • pci64.bin firmware must be present in /lib/firmware/.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/firmware.h>
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

/* ========================================================================
 * Constants
 * ======================================================================== */

#define DRIVER_NAME     "maestro2em"
#define MAESTRO_VENDOR  0x125D
#define MAESTRO_DEVICE  0x1978

/* WaveProcessor index / data ports (relative to BAR0) */
#define ESM_INDEX               0x02
#define ESM_DATA                0x00

/* AC97 codec bridge */
#define ESM_AC97_INDEX          0x30
#define ESM_AC97_DATA           0x32

/* ASSP control registers */
#define ASSP_CONTROL_A          0xA2
#define ASSP_CONTROL_B          0xA4
#define ASSP_CONTROL_C          0xA6

/* Host IRQ / config */
#define ESM_PORT_HOST_IRQ       0x18
#define ESM_CONFIG_A            0x50
#define ESM_LEGACY_AUDIO_CONTROL 0x40

/* Host IRQ enable bits */
#define ESM_HIRQ_DSIE           BIT(2)
#define ESM_HIRQ_MPU401         BIT(1)
#define ESM_HIRQ_HW_VOLUME      BIT(6)
#define SAM_INTERRUPT           BIT(3)

/* Host IRQ status (io_base + 0x1A) */
#define ESM_SOUND_IRQ           0x04
#define ESM_HWVOL_IRQ           0x40

/* ISIS / SAM communication ports */
#define ISIS_DATA               0x46
#define ISIS_ADDRESS            0x44
#define ISIS_DELAY_US           100     /* pre-access delay (µs)  */

/* GPIO */
#define ESM_GPIO_DATA           0x60
#define ESM_GPIO_MASK           0x64
#define ESM_GPIO_DIR            0x68

/* APU engine */
#define NR_APUS                 64

/* Stream format flags */
#define ESS_FMT_STEREO          0x01
#define ESS_FMT_16BIT           0x02

#define MAESTRO_MODE_PLAY       0
#define MAESTRO_MODE_CAPTURE    1

/* Bob timer */
#define ESS_SYSCLK              50000000UL
#define MAESTRO_BOB_FREQ        200
#define MAESTRO_BOB_FREQ_MAX    800

/*
 * Reference clock used in rate computation.
 * chip->clock is in Hz (sample-rate domain), not the raw 50 MHz oscillator.
 * The WaveProcessor rate register is:  (sample_rate << 16) / chip->clock
 * At 48 kHz reference, a 48 kHz stream yields 0x10000 (full-rate).
 */
#define MAESTRO_CLOCK_REF       48000

/* DMA pool: first 512 bytes reserved by hardware */
#define DMA_POOL_RESERVED       512
/* WaveCache alignment requirement */
#define WAVECACHE_ALIGN         64

/* ========================================================================
 * Data structures
 * ======================================================================== */

/**
 * struct maestro_mem_chunk - one allocation unit inside the DMA pool
 * @buf:   kernel virtual address
 * @addr:  bus (DMA) address
 * @size:  byte length of this chunk
 * @empty: non-zero → chunk is free
 * @list:  linkage in maestro.buf_list
 */
struct maestro_mem_chunk {
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

	/* ── DMA pool ──────────────────────────────────────────────── */
	void            *dma_area;     /* kernel VA of pool             */
	dma_addr_t       dma_addr;     /* bus address                   */
	size_t           dma_bytes;    /* total pool size               */
	struct list_head buf_list;     /* free/used chunks              */

	/* ── ALSA objects ───────────────────────────────────────────── */
	struct snd_pcm  *pcm;
	struct snd_ac97 *ac97;

	/* ── Locking ────────────────────────────────────────────────── */
	spinlock_t       reg_lock;        /* WaveProcessor register bus    */
	spinlock_t       substream_lock;  /* active-stream list + bob      */

	/* ── APU allocation bitmap ──────────────────────────────────── */
	u8               apu[NR_APUS];

	/* ── SAM memory-management table address (4 bytes) ──────────── */
	u8               MMT_addr[4];

	/* ── Bob timer ───────────────────────────────────────────────── */
	atomic_t         bobclient;
	int              bob_freq;
	int              clock;          /* sample-rate reference (48000) */

	/* ── Active PCM channels ─────────────────────────────────────── */
	struct list_head substream_list;
};

/**
 * struct maestro_pcm_channel - per-stream state
 */
struct maestro_pcm_channel {
	struct maestro            *chip;
	struct snd_pcm_substream  *substream;

	int running;
	int mode;          /* MAESTRO_MODE_PLAY / MAESTRO_MODE_CAPTURE  */

	u8  apu[4];        /* APU indices (0/1 play L/R; 2/3 capture)   */
	u8  apu_mode[4];   /* APU trigger mode per slot                  */
	u16 base[4];       /* APU start-pointer register cache           */

	struct maestro_mem_chunk *memory;   /* DMA chunk from pool        */

	unsigned char   fmt;          /* ESS_FMT_* flags                  */
	unsigned int    wav_shift;    /* word→byte shift for ptr math      */

	unsigned int    hwptr;        /* last-seen hardware byte offset    */
	unsigned int    count;        /* accumulated bytes since last period*/
	unsigned int    dma_size;     /* total buffer bytes                */
	unsigned int    frag_size;    /* period bytes                      */

	int             bob_freq;     /* required bob rate for this stream */

	spinlock_t      lock;
	struct list_head list;
};

/* ========================================================================
 * WaveProcessor register helpers
 * ======================================================================== */

/* Unlocked — caller must hold chip->reg_lock */
static inline u16 __maestro_read(struct maestro *chip, u16 reg)
{
	outw(reg, chip->io_base + ESM_INDEX);
	return inw(chip->io_base + ESM_DATA);
}

static inline void __maestro_write(struct maestro *chip, u16 reg, u16 val)
{
	outw(reg, chip->io_base + ESM_INDEX);
	outw(val, chip->io_base + ESM_DATA);
}

/* Locked public wrappers */
static u16 maestro_read(struct maestro *chip, u16 reg)
{
	unsigned long flags;
	u16 val;

	spin_lock_irqsave(&chip->reg_lock, flags);
	val = __maestro_read(chip, reg);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return val;
}

static void maestro_write(struct maestro *chip, u16 reg, u16 val)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	__maestro_write(chip, reg, val);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* ========================================================================
 * APU helpers
 * ======================================================================== */

/* All APU helpers are unlocked; callers must hold chip->reg_lock. */

static inline void apu_set_register(struct maestro *chip,
				     u16 apu, u16 reg, u16 val)
{
	__maestro_write(chip, reg + (apu << 4), val);
}

static inline u16 apu_get_register(struct maestro *chip, u16 apu, u16 reg)
{
	return __maestro_read(chip, reg + (apu << 4));
}

static inline void wave_set_register(struct maestro *chip, u16 reg, u16 val)
{
	__maestro_write(chip, 0x01FC, reg);
	__maestro_write(chip, 0x01FD, val);
}

/**
 * maestro_alloc_apu_pair() - reserve a stereo APU pair
 * Returns the first APU index, or -EBUSY.
 */
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

/* ========================================================================
 * AC97 codec access
 *
 * BUG FIX (v1): the original code held chip->reg_lock (a spinlock) while
 * calling msleep().  Sleeping inside a spinlock is forbidden in the kernel.
 * Fix: remove the spinlock from AC97 callbacks entirely.  The AC97 layer
 * serialises its own accesses with an internal mutex; no additional locking
 * is required here.  Short udelay() is sufficient for the codec ready-poll.
 * ======================================================================== */

/**
 * snd_maestro_ac97_wait() - spin-poll AC97 bus ready (bit 0 of INDEX port)
 *
 * May be called from process context only (AC97 layer guarantee).
 * Total worst-case wait: 1000 × 50 µs = 50 ms.
 */
static int snd_maestro_ac97_wait(struct maestro *chip)
{
	int timeout = 1000;

	while (timeout-- > 0) {
		if (!(inb(chip->io_base + ESM_AC97_INDEX) & 1))
			return 0;
		udelay(50);
	}
	dev_warn(&chip->pci->dev, "AC97 bus not ready (timeout)\n");
	return -ETIMEDOUT;
}

static void maestro_ac97_write(struct snd_ac97 *ac97,
				unsigned short reg, unsigned short val)
{
	struct maestro *chip = ac97->private_data;

	snd_maestro_ac97_wait(chip);
	outw(val, chip->io_base + ESM_AC97_DATA);
	outb(reg,  chip->io_base + ESM_AC97_INDEX);
	udelay(50);
}

static unsigned short maestro_ac97_read(struct snd_ac97 *ac97,
					 unsigned short reg)
{
	struct maestro *chip = ac97->private_data;
	u16 data = 0;

	snd_maestro_ac97_wait(chip);
	outb(reg | 0x80, chip->io_base + ESM_AC97_INDEX);
	udelay(50);

	if (!snd_maestro_ac97_wait(chip))
		data = inw(chip->io_base + ESM_AC97_DATA);

	return data;
}

static const struct snd_ac97_bus_ops maestro_ac97_bus_ops = {
	.write = maestro_ac97_write,
	.read  = maestro_ac97_read,
};

/* ========================================================================
 * DMA pool management
 *
 * BUG FIX (v1): used snd_dma_alloc_pages() with manual dmab->dev setup —
 * an API that changed across kernel versions.  Replaced with a direct
 * dma_alloc_coherent() call which is stable and unambiguous.
 * ======================================================================== */

/**
 * maestro_init_dmabuf() - allocate and partition the reserved DMA pool
 * @total_kbytes: pool size in KiB (typically 512)
 *
 * The WaveCache requires all audio buffers to reside within 28-bit
 * (256 MiB) physical address space.  We allocate one contiguous coherent
 * region and sub-allocate from it.
 */
static int maestro_init_dmabuf(struct maestro *chip, int total_kbytes)
{
	struct maestro_mem_chunk *chunk;
	size_t size = (size_t)total_kbytes * 1024;

	chip->dma_area = dma_alloc_coherent(&chip->pci->dev, size,
					    &chip->dma_addr, GFP_KERNEL);
	if (!chip->dma_area) {
		dev_err(&chip->pci->dev,
			"Cannot allocate %d KiB coherent DMA buffer\n",
			total_kbytes);
		return -ENOMEM;
	}
	chip->dma_bytes = size;

	/* Verify 28-bit (256 MiB) WaveCache constraint */
	if ((chip->dma_addr + chip->dma_bytes - 1) >> 28) {
		dev_err(&chip->pci->dev,
			"DMA pool end 0x%llx exceeds 28-bit limit\n",
			(unsigned long long)(chip->dma_addr + chip->dma_bytes - 1));
		dma_free_coherent(&chip->pci->dev, chip->dma_bytes,
				  chip->dma_area, chip->dma_addr);
		chip->dma_area = NULL;
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&chip->buf_list);

	/* Zero the hardware-reserved head block */
	memset(chip->dma_area, 0, DMA_POOL_RESERVED);

	/* Create one large free chunk covering the usable region */
	chunk = kzalloc(sizeof(*chunk), GFP_KERNEL);
	if (!chunk) {
		dma_free_coherent(&chip->pci->dev, chip->dma_bytes,
				  chip->dma_area, chip->dma_addr);
		chip->dma_area = NULL;
		return -ENOMEM;
	}

	chunk->buf   = (char *)chip->dma_area + DMA_POOL_RESERVED;
	chunk->addr  = chip->dma_addr         + DMA_POOL_RESERVED;
	chunk->size  = chip->dma_bytes        - DMA_POOL_RESERVED;
	chunk->empty = 1;
	list_add_tail(&chunk->list, &chip->buf_list);

	dev_info(&chip->pci->dev, "DMA pool: %d KiB @ 0x%llx\n",
		 total_kbytes, (unsigned long long)chip->dma_addr);
	return 0;
}

/**
 * maestro_new_memory() - first-fit allocation from the DMA pool
 * @size: requested size in bytes (will be rounded up to WAVECACHE_ALIGN)
 *
 * Must NOT be called from interrupt context (uses GFP_ATOMIC for the
 * descriptor but is otherwise safe).
 */
static struct maestro_mem_chunk *maestro_new_memory(struct maestro *chip,
						     int size)
{
	struct maestro_mem_chunk *chunk;

	if (size <= 0)
		return NULL;

	size = ALIGN(size, WAVECACHE_ALIGN);

	list_for_each_entry(chunk, &chip->buf_list, list) {
		if (!chunk->empty || chunk->size < size)
			continue;

		if (chunk->size > size) {
			/* Split: create a new free remainder chunk */
			struct maestro_mem_chunk *rem;

			rem = kzalloc(sizeof(*rem), GFP_ATOMIC);
			if (!rem)
				return NULL;

			rem->buf   = chunk->buf  + size;
			rem->addr  = chunk->addr + size;
			rem->size  = chunk->size - size;
			rem->empty = 1;
			chunk->size = size;
			list_add(&rem->list, &chunk->list);
		}

		chunk->empty = 0;
		return chunk;
	}

	dev_err(&chip->pci->dev, "DMA pool: no free chunk >= %d bytes\n", size);
	return NULL;
}

/**
 * maestro_free_memory() - return a chunk to the pool and coalesce neighbours
 */
static void maestro_free_memory(struct maestro *chip,
				struct maestro_mem_chunk *buf)
{
	struct maestro_mem_chunk *neighbour;

	if (!buf)
		return;

	buf->empty = 1;

	/* Coalesce with the following chunk */
	if (!list_is_last(&buf->list, &chip->buf_list)) {
		neighbour = list_next_entry(buf, list);
		if (neighbour->empty) {
			buf->size += neighbour->size;
			list_del(&neighbour->list);
			kfree(neighbour);
		}
	}

	/* Coalesce with the preceding chunk */
	if (buf->list.prev != &chip->buf_list) {
		neighbour = list_prev_entry(buf, list);
		if (neighbour->empty) {
			neighbour->size += buf->size;
			list_del(&buf->list);
			kfree(buf);
		}
	}
}

/**
 * maestro_free_dmabuf() - tear down the entire DMA pool
 */
static void maestro_free_dmabuf(struct maestro *chip)
{
	struct maestro_mem_chunk *chunk, *tmp;

	if (!chip->dma_area)
		return;

	list_for_each_entry_safe(chunk, tmp, &chip->buf_list, list) {
		list_del(&chunk->list);
		kfree(chunk);
	}

	dma_free_coherent(&chip->pci->dev, chip->dma_bytes,
			  chip->dma_area, chip->dma_addr);
	chip->dma_area = NULL;
}

/* ========================================================================
 * ISIS / SAM co-processor communication
 * ======================================================================== */

/* ── Raw unlocked primitives (caller holds chip->reg_lock) ── */

static void __isis_write_control(struct maestro *chip, u8 data)
{
	int timeout = 100000;

	outw(0x0001, chip->io_base + ISIS_ADDRESS);
	while ((inw(chip->io_base + ISIS_DATA) & BIT(6)) && --timeout > 0)
		cpu_relax();

	outb(data, chip->io_base + ISIS_DATA);
}

static u8 __isis_read_control(struct maestro *chip)
{
	outb(0x01, chip->io_base + ISIS_ADDRESS);
	return inb(chip->io_base + ISIS_DATA);
}

static void __isis_write_data8(struct maestro *chip, u8 data)
{
	int timeout = 100000;

	outw(0x0000, chip->io_base + ISIS_ADDRESS);
	while ((inw(chip->io_base + ISIS_DATA) & BIT(6)) && --timeout > 0)
		cpu_relax();

	outb(data, chip->io_base + ISIS_DATA);
}

static u8 __isis_read_data8(struct maestro *chip)
{
	outb(0x00, chip->io_base + ISIS_ADDRESS);
	return inb(chip->io_base + ISIS_DATA);
}

static void __isis_burstwrite_data16(struct maestro *chip,
				     const u16 *data, u16 length)
{
	u16 i;

	outw(0x0002, chip->io_base + ISIS_ADDRESS);
	for (i = 0; i < length; i++)
		outw(data[i], chip->io_base + ISIS_DATA);
}

/* ── Locked public wrappers with pre-access delays ── */

static void isis_write_control(struct maestro *chip, u8 data)
{
	unsigned long flags;

	udelay(ISIS_DELAY_US);
	spin_lock_irqsave(&chip->reg_lock, flags);
	__isis_write_control(chip, data);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static u8 isis_read_control(struct maestro *chip)
{
	unsigned long flags;
	u8 val;

	udelay(ISIS_DELAY_US);
	spin_lock_irqsave(&chip->reg_lock, flags);
	val = __isis_read_control(chip);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return val;
}

static void isis_write_data8(struct maestro *chip, u8 data)
{
	unsigned long flags;

	udelay(ISIS_DELAY_US);
	spin_lock_irqsave(&chip->reg_lock, flags);
	__isis_write_data8(chip, data);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static u8 isis_read_data8(struct maestro *chip)
{
	unsigned long flags;
	u8 val;

	udelay(ISIS_DELAY_US);
	spin_lock_irqsave(&chip->reg_lock, flags);
	val = __isis_read_data8(chip);
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

/**
 * isis_wait_control_bit7() - poll SAM handshake bit
 * @want_set: 1 = wait for bit7 to be set; 0 = wait for bit7 to be clear
 */
static int isis_wait_control_bit7(struct maestro *chip, int want_set)
{
	int timeout = 100000;

	while (timeout-- > 0) {
		int set = !!(isis_read_control(chip) & BIT(7));

		if (set == want_set)
			return 0;
		udelay(10);
	}

	dev_err(&chip->pci->dev, "Timeout waiting for SAM bit7=%d\n",
		want_set);
	return -ETIMEDOUT;
}

/*
 * SAM bootstrap microcode.
 * Loaded verbatim from the Guillemot reference driver; do not modify.
 */
static const u16 samBoot[] = {
	0xD0CE, 0x0111, 0xD0CE, 0x01D5, 0x0001, 0x0003, 0x0004, 0x0006,
	0x0001, 0x0003, 0x0002, 0x0002, 0x0006, 0x0002, 0x0001, 0x0006,
	0x0006, 0x7A0C, 0xE628, 0x0001, 0xD448, 0x1010, 0xC4CB, 0xD1CB,
	0xE2FE, 0x4F01, 0xE3FC, 0x4E0D, 0xE0FA, 0x4700, 0x8407, 0xD148,
	0x0104, 0x9107, 0x7A08, 0x7A09, 0xC590, 0xD1CB, 0xE2FE, 0x4F01,
	0xE3EE, 0xC74D, 0x6DFA, 0xD44A, 0x012E, 0xC449, 0x7816, 0x7819,
	0x7821, 0x781D, 0x782E, 0x7830, 0x7835, 0x783A, 0x783F, 0x7849,
	0x784C, 0x786E, 0x786D, 0x7914, 0x01F8, 0x7A10, 0x7A11, 0x7915,
	0x0000, 0x7913, 0x0007, 0x7A12, 0xD1CA, 0xC44F, 0xC4C4, 0xD0CE,
	0x01CB, 0xC64F, 0xC54F, 0xC44F, 0xCB4C, 0xD5C4, 0x78C4, 0xC64F,
	0xC74F, 0xCF4C, 0xC64F, 0xC54F, 0xCB4C, 0x3D09, 0xC64F, 0xC54F,
	0xCB4C, 0x3D08, 0xD449, 0x0130, 0xE302, 0xC480, 0x786C, 0xCF80,
	0x78B2, 0xC04F, 0xC4C9, 0x7867, 0xC64F, 0xC54F, 0xCB4C, 0xC04F,
	0xC5CB, 0x78A9, 0xC54F, 0xC44F, 0xC94A, 0xD1CE, 0x8405, 0x785B,
	0xC54F, 0xC44F, 0xC94A, 0xD1CE, 0x8406, 0x7855, 0xC74F, 0xC64F,
	0xCD4E, 0xC74F, 0xC54F, 0xCB4E, 0xC74F, 0xC44F, 0xC94E, 0xD1CF,
	0x7892, 0xC64F, 0xC54F, 0xCB4C, 0xC549, 0xC04F, 0x0001, 0x0400,
	0xC4CB, 0x0115, 0x0406, 0xC04F, 0xD0C1, 0x7D01, 0x6CFC, 0xD0CA,
	0x8418, 0x0001, 0xC4CB, 0xD1C9, 0x0001, 0x840C, 0xE901, 0x0000,
	0x7803, 0xE911, 0xD048, 0xFFFF, 0x7B00, 0xE920, 0xD1C8, 0xC04F,
	0xC14F, 0xC24F, 0xC34F, 0xC44F, 0xC54F, 0xC64F, 0xC74F, 0xD1CA,
	0x8704, 0x0001, 0x0410, 0xC4CB, 0xC54F, 0xC44F, 0xC94A, 0x3C0D,
	0xC54F, 0xC44F, 0xC94A, 0x3C0F, 0xC54F, 0xC44F, 0xC94A, 0x3C0E,
	0x0001, 0xD448, 0x2010, 0xD548, 0x3010, 0xD749, 0x013A, 0xE304,
	0xD448, 0x1010, 0xD548, 0x1010, 0xC4CB, 0xC5CB, 0x0006, 0xC4CB,
	0x7B0D, 0xE3FE, 0x78B5, 0x0006, 0xC4CB, 0x0001, 0xC5C9, 0x3510,
	0xE2FD, 0xCA49, 0x0006, 0xC5CB, 0x78AB, 0xC74D, 0xC64D, 0xC54D,
	0xC44D, 0xC34D, 0xC24D, 0xC14D, 0xC04D, 0x7A05, 0x840C, 0x4100,
	0xE101, 0x4104, 0xE301, 0x4201, 0xE501, 0x4302, 0xD94A, 0xD94B,
	0x3C0C, 0x0001, 0x0400, 0xC4CB, 0xD0C8, 0x0110, 0x0406, 0xC0C1,
	0xC04D, 0x7C01, 0x6CFC, 0xD0CF, 0x013B, 0xD448, 0x55AA, 0x78D3,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

/**
 * maestro_upload_firmware() - initialise the ISIS SAM co-processor
 *
 * Follows the Guillemot reference driver initialisation sequence exactly.
 * Requires firmware "pci64.bin" in /lib/firmware/.
 * May sleep — must be called from process context only.
 */
static int maestro_upload_firmware(struct maestro *chip)
{
	const struct firmware *fw = NULL;
	u16 w, *fw_words = NULL;
	size_t payload_words;
	unsigned long io = chip->io_base;
	u8 resp;
	int err, i;

	/* Preparation sequences (SAM protocol) */
	static const u8 prep_seq[]  = {
		0x00, 0x00, 0x00, 0x0B, 0x00, 0x02,
		0x00, 0x00, 0x57, 0x6B
	};
	static const u8 post_seq[]  = { 0x09, 0x00, 0x02 };

	dev_info(&chip->pci->dev, "ISIS SAM: initialising\n");

	/* Mask SAM interrupt while we programme the co-processor */
	w = inw(io + ESM_PORT_HOST_IRQ) & ~SAM_INTERRUPT;
	outw(w, io + ESM_PORT_HOST_IRQ);

	/* Enable MPU-401 decode in PCI config space */
	pci_read_config_word(chip->pci, ESM_CONFIG_A, &w);
	w |= 0x0018;
	pci_write_config_word(chip->pci, ESM_CONFIG_A, w);

	/* GPIO — select external clock source */
	outw(0x0193, io + ESM_GPIO_MASK);
	outw(0x0E64, io + ESM_GPIO_DIR);
	w = (inw(io + ESM_GPIO_DATA) & 0xFF9F) | 0x0024;
	outw(w, io + ESM_GPIO_DATA);

	/* PLD configuration */
	outw(0x0DFF, io + ESM_GPIO_MASK);
	outw(inw(io + ESM_GPIO_DATA) | 0x0200, io + ESM_GPIO_DATA);
	outw(0x0FFF, io + ESM_GPIO_MASK);

	/* Reset SAM */
	isis_write_control(chip, 0x70);
	isis_write_data8(chip, 0x11);
	msleep(10);

	/* Upload bootstrap */
	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;

	isis_burstwrite_data16(chip, samBoot, ARRAY_SIZE(samBoot));

	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;

	/* Three-step boot handshake */
	isis_write_control(chip, 0x04);
	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;

	isis_write_control(chip, 0x00);
	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;

	isis_write_control(chip, 0x05);
	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;

	/* Preparation sequence for firmware upload */
	for (i = 0; i < ARRAY_SIZE(prep_seq); i++) {
		isis_write_control(chip, prep_seq[i]);
		err = isis_wait_control_bit7(chip, 1);
		if (err)
			return err;
	}
	msleep(10);

	/* Load the main firmware image */
	err = request_firmware(&fw, "pci64.bin", &chip->pci->dev);
	if (err) {
		dev_err(&chip->pci->dev,
			"Cannot load pci64.bin (err=%d); "
			"basic AC97 audio may still function\n", err);
		return err;
	}

	if (fw->size <= 0x400) {
		dev_err(&chip->pci->dev,
			"pci64.bin too small (%zu bytes)\n", fw->size);
		err = -EINVAL;
		goto out_release_fw;
	}

	/* Skip the 1 KiB header */
	payload_words = (fw->size - 0x400) / sizeof(u16);

	fw_words = kmemdup(fw->data + 0x400,
			   payload_words * sizeof(u16), GFP_KERNEL);
	if (!fw_words) {
		err = -ENOMEM;
		goto out_release_fw;
	}

	dev_info(&chip->pci->dev, "Uploading %zu firmware words\n",
		 payload_words);

	isis_burstwrite_data16(chip, fw_words, (u16)payload_words);
	kfree(fw_words);
	release_firmware(fw);
	fw = NULL;

	/* Post-upload handshake */
	for (i = 0; i < ARRAY_SIZE(post_seq); i++) {
		err = isis_wait_control_bit7(chip, 1);
		if (err)
			return err;
		isis_write_control(chip, post_seq[i]);
	}

	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;

	/* Re-mask SAM interrupt */
	w = inw(io + ESM_PORT_HOST_IRQ) & ~SAM_INTERRUPT;
	outw(w, io + ESM_PORT_HOST_IRQ);

	/* Switch SAM to UART mode */
	isis_write_control(chip, 0x3F);
	msleep(10);

	/* Wait for SAM "ready" response (0xFE) */
	err = isis_wait_control_bit7(chip, 0);
	if (err)
		return err;

	resp = isis_read_data8(chip);
	if (resp != 0xFE)
		dev_warn(&chip->pci->dev,
			 "Unexpected SAM ready byte: 0x%02x\n", resp);

	/* Retrieve MMT address (4 bytes) */
	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;
	isis_write_control(chip, 0x03);

	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;
	isis_write_data8(chip, 0x00);

	for (i = 0; i < 4; i++) {
		err = isis_wait_control_bit7(chip, 0);
		if (err)
			return err;
		chip->MMT_addr[i] = isis_read_data8(chip);
	}

	/* Final SAM configuration commands */
	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;
	isis_write_control(chip, 0x05);

	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;
	isis_write_data8(chip, 0x01);

	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;
	isis_write_control(chip, 0x2C);

	err = isis_wait_control_bit7(chip, 1);
	if (err)
		return err;
	isis_write_data8(chip, 0x00);

	/* Trigger self-test interrupt; expected response 0x88 */
	isis_write_control(chip, 0x48);
	isis_write_data8(chip, 0x00);

	err = isis_wait_control_bit7(chip, 0);
	if (err)
		return err;

	resp = isis_read_data8(chip);
	if (resp != 0x88)
		dev_warn(&chip->pci->dev,
			 "SAM self-test response 0x%02x (expected 0x88)\n",
			 resp);

	/* Unmute output via GPIO */
	outw(0x07FF, io + ESM_GPIO_MASK);
	outw(inw(io + ESM_GPIO_DATA) | BIT(11), io + ESM_GPIO_DATA);
	outw(0x0FFF, io + ESM_GPIO_MASK);

	dev_info(&chip->pci->dev, "ISIS SAM: initialisation complete\n");
	return 0;

out_release_fw:
	release_firmware(fw);
	return err;
}

/* ========================================================================
 * Bob timer
 *
 * BUG FIX (v1): maestro_bob_start/stop used __maestro_read/write (unlocked)
 * but their callers only held substream_lock, not reg_lock.  Fixed by making
 * bob_start/stop acquire reg_lock themselves.
 *
 * Lock ordering is always:  substream_lock  →  reg_lock  (never reversed).
 * ======================================================================== */

/**
 * maestro_bob_stop() - disable the Bob periodic timer
 * Acquires reg_lock internally.
 */
static void maestro_bob_stop(struct maestro *chip)
{
	unsigned long flags;
	u16 reg;

	spin_lock_irqsave(&chip->reg_lock, flags);

	reg = __maestro_read(chip, 0x11) & ~0x0001U;
	__maestro_write(chip, 0x11, reg);

	reg = __maestro_read(chip, 0x17) & ~0x0001U;
	__maestro_write(chip, 0x17, reg);

	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/**
 * maestro_bob_start() - program and enable the Bob periodic timer
 * Acquires reg_lock internally.
 */
static void maestro_bob_start(struct maestro *chip)
{
	unsigned long flags;
	int prescale, divide;

	/* Compute dividers from chip->bob_freq (already set by caller) */
	for (prescale = 5; prescale < 12; prescale++)
		if (chip->bob_freq > (int)(ESS_SYSCLK >> (prescale + 9)))
			break;

	divide = 1;
	while (prescale > 5 && divide < 32) {
		prescale--;
		divide <<= 1;
	}
	divide >>= 1;

	for (; divide < 31; divide++)
		if (chip->bob_freq >
		    (int)((ESS_SYSCLK >> (prescale + 9)) / (divide + 1)))
			break;

	if (divide == 0) {
		divide++;
		if (prescale > 5)
			prescale--;
	} else if (divide > 1) {
		divide--;
	}

	spin_lock_irqsave(&chip->reg_lock, flags);

	__maestro_write(chip, 6, 0x9000 | (prescale << 5) | divide);
	__maestro_write(chip, 0x11, __maestro_read(chip, 0x11) | 1);
	__maestro_write(chip, 0x17, __maestro_read(chip, 0x17) | 1);

	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/**
 * maestro_bob_inc() - register a new Bob timer client
 * @freq: minimum acceptable timer frequency (Hz)
 *
 * Must be called with chip->substream_lock held.
 */
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

/**
 * maestro_bob_dec() - deregister a Bob timer client
 *
 * Must be called with chip->substream_lock held.
 * Recomputes the optimal frequency or stops the timer if no clients remain.
 */
static void maestro_bob_dec(struct maestro *chip)
{
	int clients = atomic_dec_return(&chip->bobclient);

	if (clients <= 0) {
		maestro_bob_stop(chip);
		atomic_set(&chip->bobclient, 0);
		return;
	}

	if (chip->bob_freq > MAESTRO_BOB_FREQ) {
		struct maestro_pcm_channel *chan;
		int max_freq = MAESTRO_BOB_FREQ;

		list_for_each_entry(chan, &chip->substream_list, list) {
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

/**
 * maestro_calc_bob_rate() - derive the required Bob frequency for a stream
 */
static int maestro_calc_bob_rate(struct maestro *chip,
				 struct maestro_pcm_channel *chan,
				 struct snd_pcm_runtime *runtime)
{
	int freq = runtime->rate * 4;

	if (chan->fmt & ESS_FMT_STEREO)
		freq <<= 1;
	if (chan->fmt & ESS_FMT_16BIT)
		freq <<= 1;
	if (chan->frag_size)
		freq /= chan->frag_size;

	return clamp(freq, MAESTRO_BOB_FREQ, MAESTRO_BOB_FREQ_MAX);
}

/* ========================================================================
 * APU programming
 * ======================================================================== */

/**
 * maestro_compute_rate() - convert a sample rate to the APU rate register
 *
 * The WaveProcessor rate register = (sample_rate << 16) / clock_reference.
 * At 48 kHz reference, 48 kHz → 0x10000 (maximum).
 */
static u32 maestro_compute_rate(struct maestro *chip, u32 freq)
{
	return (freq << 16) / chip->clock;
}

static void maestro_apu_set_freq(struct maestro *chip, int apu, int freq)
{
	apu_set_register(chip, apu, 2,
			 (apu_get_register(chip, apu, 2) & 0x00FF) |
			 ((freq & 0xFF) << 8) | 0x10);
	apu_set_register(chip, apu, 3, freq >> 8);
}

/* Caller must hold chip->reg_lock */
static inline void maestro_trigger_apu(struct maestro *chip,
					int apu, int mode)
{
	u16 v = apu_get_register(chip, apu, 0);

	v = (v & 0xFF0F) | ((mode & 0xF) << 4);
	apu_set_register(chip, apu, 0, v);
}

/**
 * maestro_program_wavecache() - set up a WaveCache channel
 * @channel: 0 (left/mono) or 1 (right)
 * @addr:    physical DMA address of the buffer
 * @capture: non-zero for capture direction
 *
 * Caller must hold chip->reg_lock.
 */
static void maestro_program_wavecache(struct maestro *chip,
				      struct maestro_pcm_channel *chan,
				      int channel, u32 addr, int capture)
{
	u32 tmpval = (addr - 0x10) & 0xFFF8;

	if (!capture) {
		if (!(chan->fmt & ESS_FMT_16BIT))
			tmpval |= 4;	/* 8-bit playback flag */
		if (chan->fmt & ESS_FMT_STEREO)
			tmpval |= 2;	/* stereo flag */
	}

	wave_set_register(chip, chan->apu[channel] << 3, tmpval);
}

/**
 * maestro_get_dma_ptr() - read current hardware DMA position (in words)
 *
 * Caller must hold chip->reg_lock (called via __maestro_read pathway).
 */
static inline unsigned int maestro_get_dma_ptr(struct maestro *chip,
						struct maestro_pcm_channel *chan)
{
	unsigned int offset;

	offset  = apu_get_register(chip, chan->apu[0], 5);
	offset -= chan->base[0];
	return offset & 0xFFFEU;	/* hardware pointer is word-granular */
}

/**
 * maestro_pcm_start() - arm all APUs for the given stream
 * Acquires reg_lock internally.
 */
static void maestro_pcm_start(struct maestro *chip,
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
			maestro_trigger_apu(chip, chan->apu[3],
					    chan->apu_mode[3]);
		}
	}

	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/**
 * maestro_pcm_stop() - disarm all APUs for the given stream
 * Acquires reg_lock internally.
 */
static void maestro_pcm_stop(struct maestro *chip,
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

/**
 * maestro_playback_setup() - program APU pair and WaveCache for playback
 *
 * ASSUMPTION: the APU register layout and WaveCache address translation
 * follow the es1968.c reference implementation.  Stereo interleaving
 * and 8-bit format bit-shifts are preserved from the original driver.
 */
static void maestro_playback_setup(struct maestro *chip,
				   struct maestro_pcm_channel *chan,
				   struct snd_pcm_runtime *runtime)
{
	u32 pa, freq;
	int high_apu = 0, channel, apu, i, size;
	unsigned long flags;

	chan->dma_size  = snd_pcm_lib_buffer_bytes(chan->substream);
	chan->frag_size = snd_pcm_lib_period_bytes(chan->substream);

	chan->wav_shift = 1;	/* base: word (2 bytes) → 1 shift */
	chan->fmt       = 0;

	if (snd_pcm_format_width(runtime->format) == 16)
		chan->fmt |= ESS_FMT_16BIT;

	if (runtime->channels > 1) {
		chan->fmt |= ESS_FMT_STEREO;
		if (chan->fmt & ESS_FMT_16BIT)
			chan->wav_shift++;	/* stereo 16-bit: 2 shifts */
	}

	chan->bob_freq = maestro_calc_bob_rate(chip, chan, runtime);
	size = chan->dma_size >> chan->wav_shift;

	if (chan->fmt & ESS_FMT_STEREO)
		high_apu = 1;

	spin_lock_irqsave(&chip->reg_lock, flags);

	for (channel = 0; channel <= high_apu; channel++) {
		apu = chan->apu[channel];

		maestro_program_wavecache(chip, chan, channel,
					  chan->memory->addr, 0);

		pa  = (u32)chan->memory->addr;
		pa -= (u32)chip->dma_addr;
		pa >>= 1;		/* byte → word offset */
		pa |= 0x00400000;	/* system RAM flag */

		if (chan->fmt & ESS_FMT_STEREO) {
			if (channel)
				pa |= 0x00800000;   /* right channel */
			if (chan->fmt & ESS_FMT_16BIT)
				pa >>= 1;
		}

		chan->base[channel] = pa & 0xFFFF;

		for (i = 0; i < 16; i++)
			apu_set_register(chip, apu, i, 0x0000);

		apu_set_register(chip, apu,  4, ((pa >> 16) & 0xFF) << 8);
		apu_set_register(chip, apu,  5, pa & 0xFFFF);
		apu_set_register(chip, apu,  6, (pa + size) & 0xFFFF);
		apu_set_register(chip, apu,  7, size);
		apu_set_register(chip, apu,  8, 0x0000);
		apu_set_register(chip, apu,  9, 0xD000);
		apu_set_register(chip, apu, 11, 0x0000);
		apu_set_register(chip, apu,  0, 0x400F);

		if (chan->fmt & ESS_FMT_16BIT)
			chan->apu_mode[channel] = 0x01;	/* 16-bit linear */
		else
			chan->apu_mode[channel] = 0x03;	/* 8-bit linear  */

		if (chan->fmt & ESS_FMT_STEREO) {
			apu_set_register(chip, apu, 10,
					 0x8F00 | (channel ? 0 : 0x10));
			chan->apu_mode[channel]++;	/* stereo variant */
		} else {
			apu_set_register(chip, apu, 10, 0x8F08);
		}
	}

	/* Enable WaveProcessor and DirectSound interrupts */
	outw(1, chip->io_base + 0x04);
	outw(inw(chip->io_base + ESM_PORT_HOST_IRQ) | ESM_HIRQ_DSIE,
	     chip->io_base + ESM_PORT_HOST_IRQ);

	spin_unlock_irqrestore(&chip->reg_lock, flags);

	/* Set sample rate (clamped to hardware range) */
	freq = clamp_val(runtime->rate, 4000U, 48000U);

	if (!(chan->fmt & ESS_FMT_16BIT) && !(chan->fmt & ESS_FMT_STEREO))
		freq >>= 1;

	freq = maestro_compute_rate(chip, freq);

	spin_lock_irqsave(&chip->reg_lock, flags);
	maestro_apu_set_freq(chip, chan->apu[0], freq);
	maestro_apu_set_freq(chip, chan->apu[1], freq);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* ========================================================================
 * PCM operations
 * ======================================================================== */

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

	chan->chip       = chip;
	chan->substream  = substream;
	chan->apu[0]     = apu1;
	chan->apu[1]     = apu1 + 1;
	chan->mode       = MAESTRO_MODE_PLAY;
	spin_lock_init(&chan->lock);
	INIT_LIST_HEAD(&chan->list);

	runtime->private_data = chan;

	runtime->hw.info =
		SNDRV_PCM_INFO_MMAP        |
		SNDRV_PCM_INFO_MMAP_VALID  |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_RESUME;
	runtime->hw.formats =
		SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE;
	runtime->hw.rates =
		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000;
	runtime->hw.rate_min         = 4000;
	runtime->hw.rate_max         = 48000;
	runtime->hw.channels_min     = 1;
	runtime->hw.channels_max     = 2;
	runtime->hw.buffer_bytes_max = chip->dma_bytes - DMA_POOL_RESERVED;
	runtime->hw.period_bytes_min = 256;
	runtime->hw.period_bytes_max = chip->dma_bytes - DMA_POOL_RESERVED;
	runtime->hw.periods_min      = 1;
	runtime->hw.periods_max      = 1024;
	runtime->hw.fifo_size        = 0;

	spin_lock_irq(&chip->substream_lock);
	list_add(&chan->list, &chip->substream_list);
	spin_unlock_irq(&chip->substream_lock);

	return 0;
}

static int maestro_playback_close(struct snd_pcm_substream *substream)
{
	struct maestro *chip = snd_pcm_substream_chip(substream);
	struct maestro_pcm_channel *chan = substream->runtime->private_data;

	if (!chan)
		return 0;

	spin_lock_irq(&chip->substream_lock);
	list_del(&chan->list);
	spin_unlock_irq(&chip->substream_lock);

	maestro_free_apu_pair(chip, chan->apu[0]);
	kfree(chan);
	substream->runtime->private_data = NULL;

	return 0;
}

static int maestro_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *hw_params)
{
	struct maestro *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct maestro_pcm_channel *chan = runtime->private_data;
	int size = params_buffer_bytes(hw_params);

	/* Re-use existing allocation if it is large enough */
	if (chan->memory) {
		if (chan->memory->size >= size) {
			runtime->dma_area  = chan->memory->buf;
			runtime->dma_addr  = chan->memory->addr;
			runtime->dma_bytes = size;
			return 0;
		}
		maestro_free_memory(chip, chan->memory);
		chan->memory = NULL;
	}

	chan->memory = maestro_new_memory(chip, size);
	if (!chan->memory)
		return -ENOMEM;

	runtime->dma_area  = chan->memory->buf;
	runtime->dma_addr  = chan->memory->addr;
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

	runtime->dma_area  = NULL;
	runtime->dma_addr  = 0;
	runtime->dma_bytes = 0;

	return 0;
}

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

	return 0;
}

static int maestro_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct maestro *chip = snd_pcm_substream_chip(substream);
	struct maestro_pcm_channel *chan = substream->runtime->private_data;
	int ret = 0;

	/*
	 * Called with local IRQs disabled by the ALSA core.
	 * Use plain spin_lock (not _irqsave) to avoid redundant flag save.
	 */
	spin_lock(&chip->substream_lock);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!chan->running) {
			maestro_bob_inc(chip, chan->bob_freq);
			chan->hwptr  = 0;
			chan->count  = 0;
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

	default:
		ret = -EINVAL;
		break;
	}

	spin_unlock(&chip->substream_lock);
	return ret;
}

static snd_pcm_uframes_t maestro_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct maestro *chip = snd_pcm_substream_chip(substream);
	struct maestro_pcm_channel *chan = substream->runtime->private_data;
	unsigned long flags;
	unsigned int ptr;

	if (!chan || !chan->memory)
		return 0;

	spin_lock_irqsave(&chip->reg_lock, flags);
	ptr = maestro_get_dma_ptr(chip, chan);
	spin_unlock_irqrestore(&chip->reg_lock, flags);

	ptr = (ptr << chan->wav_shift) % chan->dma_size;
	return bytes_to_frames(substream->runtime, ptr);
}

/*
 * BUG FIX (v1): ".ioctl = snd_pcm_lib_ioctl" was removed from the
 * snd_pcm_ops struct in Linux 5.12.  Omitting it is the correct approach
 * for all kernels ≥ 5.12.
 */
static const struct snd_pcm_ops maestro_playback_ops = {
	.open      = maestro_playback_open,
	.close     = maestro_playback_close,
	.hw_params = maestro_hw_params,
	.hw_free   = maestro_hw_free,
	.prepare   = maestro_pcm_prepare,
	.trigger   = maestro_pcm_trigger,
	.pointer   = maestro_pcm_pointer,
};

/* ========================================================================
 * Interrupt handler
 * ======================================================================== */

/**
 * maestro_update_pcm() - advance stream position, fire period-elapsed
 *
 * Called from the ISR with chip->substream_lock held.
 * Temporarily drops substream_lock around snd_pcm_period_elapsed() to
 * avoid deadlocks with the ALSA PCM state machine (same pattern as
 * the upstream es1968.c driver).
 */
static void maestro_update_pcm(struct maestro *chip,
				struct maestro_pcm_channel *chan)
{
	struct snd_pcm_substream *subs = chan->substream;
	unsigned long flags;
	unsigned int hwptr, diff;

	if (!subs || !chan->running)
		return;

	spin_lock_irqsave(&chip->reg_lock, flags);
	hwptr = maestro_get_dma_ptr(chip, chan);
	spin_unlock_irqrestore(&chip->reg_lock, flags);

	hwptr  = (hwptr << chan->wav_shift) % chan->dma_size;
	diff   = (chan->dma_size + hwptr - chan->hwptr) % chan->dma_size;
	chan->hwptr  = hwptr;
	chan->count += diff;

	if (chan->count >= chan->frag_size) {
		chan->count %= chan->frag_size;
		spin_unlock(&chip->substream_lock);
		snd_pcm_period_elapsed(subs);
		spin_lock(&chip->substream_lock);
	}
}

static irqreturn_t maestro_interrupt(int irq, void *dev_id)
{
	struct maestro *chip = dev_id;
	struct maestro_pcm_channel *chan;
	u8 event;

	if (!chip)
		return IRQ_NONE;

	event = inb(chip->io_base + 0x1A);
	if (!event)
		return IRQ_NONE;

	/* Acknowledge WaveProcessor interrupt */
	outw(inw(chip->io_base + 0x04) & 1, chip->io_base + 0x04);

	/* Acknowledge all interrupt sources */
	outb(0xFF, chip->io_base + 0x1A);

	if (event & ESM_SOUND_IRQ) {
		spin_lock(&chip->substream_lock);
		list_for_each_entry(chan, &chip->substream_list, list)
			maestro_update_pcm(chip, chan);
		spin_unlock(&chip->substream_lock);
	}

	if (event & ESM_HWVOL_IRQ) {
		/* TODO: hardware volume wheel support */
	}

	if (event & SAM_INTERRUPT) {
		/* TODO: SAM event handling */
	}

	return IRQ_HANDLED;
}

/* ========================================================================
 * Chip and codec initialisation
 * ======================================================================== */

static void maestro_chip_init(struct maestro *chip)
{
	u16 w;
	unsigned long io = chip->io_base;

	/* Configure legacy audio */
	w = inw(io + ESM_LEGACY_AUDIO_CONTROL);
	w = (w & ~0x000FU) | 0x0001;
	outw(w, io + ESM_LEGACY_AUDIO_CONTROL);

	/* Mask all host interrupts at startup */
	w = inw(io + ESM_PORT_HOST_IRQ);
	w &= ~(ESM_HIRQ_DSIE | ESM_HIRQ_MPU401 |
	       ESM_HIRQ_HW_VOLUME | SAM_INTERRUPT);
	outw(w, io + ESM_PORT_HOST_IRQ);

	/*
	 * chip->clock is the sample-rate reference used in rate computation
	 * (not the 50 MHz oscillator frequency).
	 */
	chip->clock = MAESTRO_CLOCK_REF;
}

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
	ac97.pci          = chip->pci;
	ac97.scaps        = AC97_SCAP_AUDIO;

	err = snd_ac97_mixer(bus, &ac97, &chip->ac97);
	if (err < 0) {
		chip->ac97 = NULL;
		return err;
	}

	dev_info(&chip->pci->dev, "AC97 codec ID: 0x%08x\n", chip->ac97->id);
	return 0;
}

/* ========================================================================
 * PCI driver
 * ======================================================================== */

static int maestro_probe(struct pci_dev *pdev,
			  const struct pci_device_id *id)
{
	struct maestro   *chip;
	struct snd_card  *card;
	struct snd_pcm   *pcm;
	resource_size_t   bar0_start, bar0_len;
	int err;

	err = pci_enable_device(pdev);
	if (err < 0)
		return err;

	pci_set_master(pdev);

	/*
	 * WaveCache requires all DMA addresses within 28-bit space.
	 * Set both the streaming and coherent masks accordingly.
	 */
	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
	if (err) {
		dev_err(&pdev->dev, "28-bit DMA not available\n");
		goto err_disable;
	}

	err = snd_card_new(&pdev->dev, -1, "maestro", THIS_MODULE, 0, &card);
	if (err < 0)
		goto err_disable;

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip) {
		err = -ENOMEM;
		goto err_card;
	}

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

	/* Claim I/O BAR */
	bar0_start = pci_resource_start(pdev, 0);
	bar0_len   = pci_resource_len(pdev, 0);

	if (!request_region(bar0_start, bar0_len, DRIVER_NAME)) {
		dev_err(&pdev->dev, "I/O region 0x%llx+0x%llx already in use\n",
			(unsigned long long)bar0_start,
			(unsigned long long)bar0_len);
		err = -EBUSY;
		goto err_free_chip;
	}
	chip->io_base = bar0_start;

	err = request_irq(pdev->irq, maestro_interrupt, IRQF_SHARED,
			  DRIVER_NAME, chip);
	if (err < 0) {
		dev_err(&pdev->dev, "Cannot request IRQ %d\n", pdev->irq);
		goto err_region;
	}
	chip->irq = pdev->irq;

	maestro_chip_init(chip);

	err = maestro_init_dmabuf(chip, 512);
	if (err < 0) {
		dev_err(&pdev->dev, "DMA pool init failed: %d\n", err);
		goto err_irq;
	}

	err = snd_pcm_new(card, "Maestro PCM", 0, 1, 0, &pcm);
	if (err < 0) {
		dev_err(&pdev->dev, "snd_pcm_new failed: %d\n", err);
		goto err_dma;
	}

	chip->pcm      = pcm;
	pcm->private_data = chip;
	strscpy(pcm->name, "Guillemot Maxi Studio ISIS", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &maestro_playback_ops);

	err = maestro_ac97_attach(chip);
	if (err < 0) {
		dev_err(&pdev->dev, "AC97 init failed: %d\n", err);
		goto err_dma;
	}

	err = maestro_upload_firmware(chip);
	if (err < 0) {
		dev_warn(&pdev->dev,
			 "Firmware load failed (%d); ISIS effects unavailable\n",
			 err);
		/* Non-fatal: basic PCM playback may still work */
	}

	strscpy(card->driver,    DRIVER_NAME,
		sizeof(card->driver));
	strscpy(card->shortname, "Guillemot Maxi Studio ISIS",
		sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "%s at 0x%lx, IRQ %d",
		 card->shortname, chip->io_base, chip->irq);

	err = snd_card_register(card);
	if (err < 0) {
		dev_err(&pdev->dev, "snd_card_register failed: %d\n", err);
		goto err_dma;
	}

	dev_info(&pdev->dev, "%s initialised successfully\n",
		 card->shortname);
	return 0;

	/* Error unwind — labels fall through in order */
err_dma:
	maestro_free_dmabuf(chip);
err_irq:
	free_irq(chip->irq, chip);
err_region:
	release_region(chip->io_base, bar0_len);
err_free_chip:
	kfree(chip);
err_card:
	snd_card_free(card);
err_disable:
	pci_disable_device(pdev);
	return err;
}

static void maestro_remove(struct pci_dev *pdev)
{
	struct maestro *chip = pci_get_drvdata(pdev);
	resource_size_t bar0_len;

	if (!chip)
		return;

	bar0_len = pci_resource_len(pdev, 0);

	/* Silence hardware before tearing down */
	outw(0, chip->io_base + ESM_PORT_HOST_IRQ);

	free_irq(chip->irq, chip);
	maestro_free_dmabuf(chip);

	/*
	 * snd_card_free() calls all registered destructors (PCM, AC97, …).
	 * chip->card is freed here; chip itself is freed immediately after.
	 */
	snd_card_free(chip->card);

	release_region(chip->io_base, bar0_len);
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

MODULE_AUTHOR("ISISALSA Project");
MODULE_DESCRIPTION("ALSA driver for Guillemot Maxi Studio ISIS (ESS Maestro-2E ES1978)");
MODULE_LICENSE("GPL v2");
MODULE_FIRMWARE("pci64.bin");
