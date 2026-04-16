/*
 * maestro2em.c - ALSA driver for Guillemot Maxi Studio ISIS
 * 
 * Based on ESS Maestro-2E (ES1968/ES1978) chipset
 * Modernized and cleaned up for current Linux kernels
 *
 * Key Features:
 * - Reserved DMA pool allocation (28-bit addressing for WaveCache)
 * - ISIS/SAM firmware loading and management
 * - AC97 codec support
 * - Playback and capture via APU (Audio Processing Unit)
 * - Bob timer for interrupt generation
 *
 * License: GPL v2
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
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
#include <sound/control.h>

/* ========================================================================
 * Hardware Register Definitions
 * ======================================================================== */

#define DRIVER_NAME "maestro2em"
#define MAESTRO_VENDOR 0x125d
#define MAESTRO_DEVICE 0x1978

/* Index/Data register pair for WaveProcessor access */
#define ESM_INDEX               0x02
#define ESM_DATA                0x00

/* AC97 codec registers */
#define ESM_AC97_INDEX          0x30
#define ESM_AC97_DATA           0x32

/* ASSP (APU) control registers */
#define ASSP_CONTROL_A          0xA2
#define ASSP_CONTROL_B          0xA4
#define ASSP_CONTROL_C          0xA6

/* Host interrupt and configuration */
#define ESM_PORT_HOST_IRQ       0x18
#define ESM_CONFIG_A            0x50
#define ESM_LEGACY_AUDIO_CONTROL 0x40

/* Host IRQ control bits */
#define ESM_HIRQ_DSIE           (1 << 2)  /* DirectSound IRQ enable */
#define ESM_HIRQ_MPU401         (1 << 1)
#define ESM_HIRQ_HW_VOLUME      (1 << 6)
#define SAM_INTERRUPT           (1 << 3)  /* SAM/ISIS firmware interrupt */

/* Host IRQ status bits (read at io_base + 0x1A) */
#define ESM_SOUND_IRQ           0x04
#define ESM_HWVOL_IRQ           0x40

/* ISIS/SAM communication ports */
#define ISIS_DATA               0x46
#define ISIS_ADDRESS            0x44
#define ISIS_PRE_READ_US        100
#define ISIS_PRE_WRITE_US       100

/* GPIO ports for SAM control */
#define ESM_GPIO_DATA           0x60
#define ESM_GPIO_MASK           0x64
#define ESM_GPIO_DIR            0x68

/* APU count */
#define NR_APUS                 64

/* Stream format flags */
#define ESS_FMT_STEREO          0x01
#define ESS_FMT_16BIT           0x02

/* Stream modes */
#define MAESTRO_MODE_PLAY       0
#define MAESTRO_MODE_CAPTURE    1

/* Bob timer configuration */
#define ESS_SYSCLK              50000000
#define MAESTRO_BOB_FREQ        200
#define MAESTRO_BOB_FREQ_MAX    800

/* ========================================================================
 * Data Structures
 * ======================================================================== */

/**
 * struct maestro_mem_chunk - DMA memory chunk in reserved pool
 * @buf: Virtual address of chunk
 * @addr: Bus/physical address
 * @size: Size in bytes
 * @empty: 1 if free, 0 if allocated
 * @list: Linked list node
 */
struct maestro_mem_chunk {
	char *buf;
	dma_addr_t addr;
	int size;
	int empty;
	struct list_head list;
};

/**
 * struct maestro - Main chip context
 */
struct maestro {
	struct snd_card *card;
	struct pci_dev *pci;
	unsigned long io_base;
	int irq;

	/* DMA pool management (es1968-style reserved pool) */
	struct snd_dma_buffer dma;
	struct list_head buf_list;

	/* PCM and codec */
	struct snd_pcm *pcm;
	struct snd_ac97 *ac97;

	/* Locks */
	spinlock_t reg_lock;
	spinlock_t substream_lock;

	/* APU management */
	u8 apu[NR_APUS];
	u8 MMT_addr[4];

	/* Bob timer state */
	atomic_t bobclient;
	int bob_freq;
	int clock;

	/* Active streams */
	struct list_head substream_list;
};

/**
 * struct maestro_pcm_channel - Per-stream state
 */
struct maestro_pcm_channel {
	struct maestro *chip;
	struct snd_pcm_substream *substream;
	
	int running;
	int mode;  /* MAESTRO_MODE_PLAY or MAESTRO_MODE_CAPTURE */

	/* APU assignment */
	u8 apu[4];
	u8 apu_mode[4];
	u16 base[4];

	/* DMA buffer from pool */
	struct maestro_mem_chunk *memory;

	/* Stream format */
	unsigned char fmt;
	unsigned int wav_shift;

	/* Buffer tracking */
	unsigned int hwptr;
	unsigned int count;
	unsigned int dma_size;
	unsigned int frag_size;

	/* Bob timer frequency for this stream */
	int bob_freq;

	spinlock_t lock;
	struct list_head list;
};

/* ========================================================================
 * Low-Level Register Access Helpers
 * ======================================================================== */

/**
 * __maestro_read - Read WaveProcessor register (unlocked)
 */
static inline u16 __maestro_read(struct maestro *chip, u16 reg)
{
	outw(reg, chip->io_base + ESM_INDEX);
	return inw(chip->io_base + ESM_DATA);
}

/**
 * maestro_read - Read WaveProcessor register (locked)
 */
static u16 maestro_read(struct maestro *chip, u16 reg)
{
	unsigned long flags;
	u16 val;
	
	spin_lock_irqsave(&chip->reg_lock, flags);
	val = __maestro_read(chip, reg);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	
	return val;
}

/**
 * __maestro_write - Write WaveProcessor register (unlocked)
 */
static inline void __maestro_write(struct maestro *chip, u16 reg, u16 val)
{
	outw(reg, chip->io_base + ESM_INDEX);
	outw(val, chip->io_base + ESM_DATA);
}

/**
 * maestro_write - Write WaveProcessor register (locked)
 */
static void maestro_write(struct maestro *chip, u16 reg, u16 val)
{
	unsigned long flags;
	
	spin_lock_irqsave(&chip->reg_lock, flags);
	__maestro_write(chip, reg, val);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/* ========================================================================
 * APU (Audio Processing Unit) Helpers
 * ======================================================================== */

/**
 * apu_set_register - Write to APU register
 */
static inline void apu_set_register(struct maestro *chip, u16 apu, 
                                    u16 reg, u16 val)
{
	__maestro_write(chip, reg + (apu << 4), val);
}

/**
 * apu_get_register - Read from APU register
 */
static inline u16 apu_get_register(struct maestro *chip, u16 apu, u16 reg)
{
	return __maestro_read(chip, reg + (apu << 4));
}

/**
 * wave_set_register - Write to WaveCache register
 */
static inline void wave_set_register(struct maestro *chip, u16 reg, u16 val)
{
	__maestro_write(chip, 0x01FC, reg);
	__maestro_write(chip, 0x01FD, val);
}

/**
 * maestro_alloc_apu_pair - Allocate a stereo APU pair
 * Returns APU number or -EBUSY if none available
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

/**
 * maestro_free_apu_pair - Free an APU pair
 */
static void maestro_free_apu_pair(struct maestro *chip, int apu)
{
	if (apu < 0 || apu + 1 >= NR_APUS)
		return;
	chip->apu[apu] = chip->apu[apu + 1] = 0;
}

/* ========================================================================
 * AC97 Codec Access
 * ======================================================================== */

/**
 * snd_maestro_ac97_wait - Wait for AC97 bus to be ready
 */
static int snd_maestro_ac97_wait(struct maestro *chip)
{
	int timeout = 100000;
	
	while (timeout-- > 0) {
		if (!(inb(chip->io_base + ESM_AC97_INDEX) & 1))
			return 0;
		udelay(1);
	}
	
	dev_warn(&chip->pci->dev, "AC97 wait timeout\n");
	return -ETIMEDOUT;
}

/**
 * maestro_ac97_write - Write to AC97 register
 */
static void maestro_ac97_write(struct snd_ac97 *ac97, unsigned short reg,
                                unsigned short val)
{
	struct maestro *chip = ac97->private_data;
	unsigned long flags;
	
	spin_lock_irqsave(&chip->reg_lock, flags);
	snd_maestro_ac97_wait(chip);
	outw(val, chip->io_base + ESM_AC97_DATA);
	outb(reg, chip->io_base + ESM_AC97_INDEX);
	msleep(1);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/**
 * maestro_ac97_read - Read from AC97 register
 */
static unsigned short maestro_ac97_read(struct snd_ac97 *ac97,
                                         unsigned short reg)
{
	struct maestro *chip = ac97->private_data;
	unsigned long flags;
	u16 data = 0;
	
	spin_lock_irqsave(&chip->reg_lock, flags);
	snd_maestro_ac97_wait(chip);
	outb(reg | 0x80, chip->io_base + ESM_AC97_INDEX);
	msleep(1);
	
	if (!snd_maestro_ac97_wait(chip)) {
		data = inw(chip->io_base + ESM_AC97_DATA);
		msleep(1);
	}
	
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return data;
}

static const struct snd_ac97_bus_ops maestro_ac97_bus_ops = {
	.write = maestro_ac97_write,
	.read  = maestro_ac97_read,
};

/* ========================================================================
 * DMA Pool Management (ES1968-style reserved pool)
 * ======================================================================== */

/**
 * maestro_init_dmabuf - Initialize reserved DMA pool
 * @chip: Chip context
 * @total_kbytes: Total pool size in kilobytes
 *
 * Allocates a contiguous DMA buffer within 28-bit address space
 * for WaveCache compatibility. Creates initial free chunk.
 */
static int maestro_init_dmabuf(struct maestro *chip, int total_kbytes)
{
	struct maestro_mem_chunk *chunk;
	size_t size = total_kbytes * 1024;
	
	/* Allocate DMA buffer with 28-bit addressing constraint */
	chip->dma.dev.type = SNDRV_DMA_TYPE_DEV;
	chip->dma.dev.dev = &chip->pci->dev;
	
	if (snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, &chip->pci->dev,
	                        size, &chip->dma) < 0) {
		dev_err(&chip->pci->dev, "Cannot allocate %d KB DMA buffer\n",
		        total_kbytes);
		return -ENOMEM;
	}
	
	/* Verify 28-bit address constraint (WaveCache limitation) */
	if ((chip->dma.addr + chip->dma.bytes - 1) & ~0x0FFFFFFFUL) {
		dev_err(&chip->pci->dev,
		        "DMA buffer at 0x%llx exceeds 28-bit limit\n",
		        (unsigned long long)chip->dma.addr);
		snd_dma_free_pages(&chip->dma);
		return -ENOMEM;
	}
	
	/* Initialize chunk list */
	INIT_LIST_HEAD(&chip->buf_list);
	
	/* Reserve first 512 bytes (hardware requirement) */
	memset(chip->dma.area, 0, 512);
	
	/* Create initial free chunk */
	chunk = kzalloc(sizeof(*chunk), GFP_KERNEL);
	if (!chunk) {
		snd_dma_free_pages(&chip->dma);
		return -ENOMEM;
	}
	
	chunk->buf = chip->dma.area + 512;
	chunk->addr = chip->dma.addr + 512;
	chunk->size = chip->dma.bytes - 512;
	chunk->empty = 1;
	list_add_tail(&chunk->list, &chip->buf_list);
	
	dev_info(&chip->pci->dev, "DMA pool: %d KB at 0x%llx\n",
	         total_kbytes, (unsigned long long)chip->dma.addr);
	
	return 0;
}

/**
 * maestro_new_memory - Allocate chunk from DMA pool
 * @chip: Chip context
 * @size: Requested size in bytes
 *
 * Uses first-fit algorithm and splits chunks as needed.
 * Size is rounded up to 64-byte alignment (WaveCache requirement).
 */
static struct maestro_mem_chunk *maestro_new_memory(struct maestro *chip,
                                                     int size)
{
	struct maestro_mem_chunk *chunk;
	
	if (!size)
		return NULL;
	
	/* Round up to 64-byte boundary (WaveCache requirement) */
	size = ALIGN(size, 64);
	
	/* Find first suitable free chunk */
	list_for_each_entry(chunk, &chip->buf_list, list) {
		if (chunk->empty && chunk->size >= size) {
			/* Split chunk if larger than needed */
			if (chunk->size > size) {
				struct maestro_mem_chunk *newc;
				
				newc = kzalloc(sizeof(*newc), GFP_ATOMIC);
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

/**
 * maestro_free_memory - Free chunk back to pool
 * @chip: Chip context
 * @buf: Chunk to free
 *
 * Marks chunk as free and coalesces with adjacent free chunks.
 */
static void maestro_free_memory(struct maestro *chip,
                                struct maestro_mem_chunk *buf)
{
	struct maestro_mem_chunk *chunk;
	
	if (!buf)
		return;
	
	buf->empty = 1;
	
	/* Coalesce with next chunk if free */
	if (buf->list.next != &chip->buf_list) {
		chunk = list_entry(buf->list.next, struct maestro_mem_chunk,
		                   list);
		if (chunk->empty) {
			buf->size += chunk->size;
			list_del(&chunk->list);
			kfree(chunk);
		}
	}
	
	/* Coalesce with previous chunk if free */
	if (buf->list.prev != &chip->buf_list) {
		chunk = list_entry(buf->list.prev, struct maestro_mem_chunk,
		                   list);
		if (chunk->empty) {
			chunk->size += buf->size;
			list_del(&buf->list);
			kfree(buf);
		}
	}
}

/**
 * maestro_free_dmabuf - Free entire DMA pool
 */
static void maestro_free_dmabuf(struct maestro *chip)
{
	struct maestro_mem_chunk *chunk, *next;
	
	if (!chip->dma.area)
		return;
	
	/* Free all chunks */
	list_for_each_entry_safe(chunk, next, &chip->buf_list, list) {
		list_del(&chunk->list);
		kfree(chunk);
	}
	
	/* Free DMA buffer */
	snd_dma_free_pages(&chip->dma);
	chip->dma.area = NULL;
}

/* ========================================================================
 * ISIS/SAM Firmware Communication
 * ======================================================================== */

/**
 * Low-level ISIS register access (must hold reg_lock)
 */
static void __isis_write_control(struct maestro *chip, u8 data)
{
	unsigned long io = chip->io_base;
	int timeout = 100000;
	
	outw(0x0001, io + ISIS_ADDRESS);
	
	/* Wait until not busy (bit 6 cleared) */
	while ((inw(io + ISIS_DATA) & (1 << 6)) && timeout-- > 0)
		cpu_relax();
	
	outb(data, io + ISIS_DATA);
}

static u8 __isis_read_control(struct maestro *chip)
{
	outb(0x01, chip->io_base + ISIS_ADDRESS);
	return inb(chip->io_base + ISIS_DATA);
}

static void __isis_write_data8(struct maestro *chip, u8 data)
{
	unsigned long io = chip->io_base;
	int timeout = 100000;
	
	outw(0x0000, io + ISIS_ADDRESS);
	
	while ((inw(io + ISIS_DATA) & (1 << 6)) && timeout-- > 0)
		cpu_relax();
	
	outb(data, io + ISIS_DATA);
}

static u8 __isis_read_data8(struct maestro *chip)
{
	outb(0x00, chip->io_base + ISIS_ADDRESS);
	return inb(chip->io_base + ISIS_DATA);
}

static void __isis_write_data16(struct maestro *chip, u16 data)
{
	outw(0x0002, chip->io_base + ISIS_ADDRESS);
	outw(data, chip->io_base + ISIS_DATA);
}

static u16 __isis_read_data16(struct maestro *chip)
{
	outb(0x02, chip->io_base + ISIS_ADDRESS);
	return inw(chip->io_base + ISIS_DATA);
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

/**
 * Public ISIS wrappers with proper locking and timing
 */
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

static void isis_burstwrite_data16(struct maestro *chip,
                                   const u16 *data, u16 length)
{
	unsigned long flags;
	
	spin_lock_irqsave(&chip->reg_lock, flags);
	__isis_burstwrite_data16(chip, data, length);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/**
 * isis_wait_control_bit7 - Wait for SAM control bit 7
 * @want_set: Wait for bit to be set (1) or cleared (0)
 */
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
	
	dev_err(&chip->pci->dev, "Timeout waiting for SAM bit7=%d\n",
	        want_set);
	return -ETIMEDOUT;
}

/**
 * SAM boot code (loaded before main firmware)
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
 * maestro_upload_firmware - Load ISIS SAM firmware
 *
 * Complex initialization sequence based on Guillemot's reference driver.
 * Requires firmware file "pci64.bin" in /lib/firmware/
 */
static int maestro_upload_firmware(struct maestro *chip)
{
	const struct firmware *fw = NULL;
	int err, i;
	u16 w, *fw_words = NULL;
	unsigned long io = chip->io_base;
	size_t payload_bytes, payload_words;
	u8 resp;

	dev_info(&chip->pci->dev, "ISIS SAM initialization starting\n");

	/* Disable SAM interrupt */
	w = inw(io + ESM_PORT_HOST_IRQ);
	w &= ~SAM_INTERRUPT;
	outw(w, io + ESM_PORT_HOST_IRQ);

	/* Enable MPU-401 decode in PCI config space */
	pci_read_config_word(chip->pci, ESM_CONFIG_A, &w);
	w |= 0x0018;
	pci_write_config_word(chip->pci, ESM_CONFIG_A, w);

	/* Configure GPIO for clock source */
	outw(0x0193, io + ESM_GPIO_MASK);
	outw(0x0E64, io + ESM_GPIO_DIR);
	w = inw(io + ESM_GPIO_DATA);
	w = (w & 0xFF9F) | 0x0024;
	outw(w, io + ESM_GPIO_DATA);

	/* PLD configuration sequence */
	outw(0x0DFF, io + ESM_GPIO_MASK);
	w = inw(io + ESM_GPIO_DATA) | 0x0200;
	outw(w, io + ESM_GPIO_DATA);
	outw(0x0FFF, io + ESM_GPIO_MASK);

	/* Reset SAM */
	isis_write_control(chip, 0x70);
	isis_write_data8(chip, 0x11);
	msleep(10);

	/* Boot SAM with bootstrap code */
	err = isis_wait_control_bit7(chip, 1);
	if (err < 0)
		return err;

	isis_burstwrite_data16(chip, samBoot, ARRAY_SIZE(samBoot));

	err = isis_wait_control_bit7(chip, 1);
	if (err < 0)
		return err;

	/* Complete boot sequence */
	isis_write_control(chip, 0x04);
	err = isis_wait_control_bit7(chip, 1);
	if (err < 0)
		return err;

	isis_write_control(chip, 0x00);
	err = isis_wait_control_bit7(chip, 1);
	if (err < 0)
		return err;

	/* Prepare for firmware upload */
	isis_write_control(chip, 0x05);
	err = isis_wait_control_bit7(chip, 1);
	if (err < 0)
		return err;

	/* Sequence of control writes for firmware mode */
	const u8 prep_sequence[] = {
		0x00, 0x00, 0x00, 0x0B, 0x00, 0x02, 0x00, 0x00, 0x57, 0x6B
	};
	
	for (i = 0; i < ARRAY_SIZE(prep_sequence); i++) {
		isis_write_control(chip, prep_sequence[i]);
		err = isis_wait_control_bit7(chip, 1);
		if (err < 0)
			return err;
	}

	msleep(10);

	/* Load main firmware file */
	err = request_firmware(&fw, "pci64.bin", &chip->pci->dev);
	if (err < 0) {
		dev_err(&chip->pci->dev, "Cannot load firmware pci64.bin: %d\n",
		        err);
		return err;
	}

	if (fw->size <= 0x400) {
		dev_err(&chip->pci->dev, "Firmware too small: %zu bytes\n",
		        fw->size);
		err = -EINVAL;
		goto out_release_fw;
	}

	/* Skip 1KB header, upload payload */
	payload_bytes = fw->size - 0x400;
	payload_words = payload_bytes / 2;

	fw_words = kmalloc(payload_words * sizeof(u16), GFP_KERNEL);
	if (!fw_words) {
		err = -ENOMEM;
		goto out_release_fw;
	}

	memcpy(fw_words, fw->data + 0x400, payload_words * sizeof(u16));

	dev_info(&chip->pci->dev, "Uploading %zu words of firmware\n",
	         payload_words);

	isis_burstwrite_data16(chip, fw_words, (u16)payload_words);
	kfree(fw_words);
	release_firmware(fw);

	/* Post-upload sequence */
	const u8 post_sequence[] = { 0x09, 0x00, 0x02 };
	
	for (i = 0; i < ARRAY_SIZE(post_sequence); i++) {
		err = isis_wait_control_bit7(chip, 1);
		if (err < 0)
			return err;
		isis_write_control(chip, post_sequence[i]);
	}

	err = isis_wait_control_bit7(chip, 1);
	if (err < 0)
		return err;

	/* Disable SAM interrupt again */
	w = inw(io + ESM_PORT_HOST_IRQ);
	w &= ~SAM_INTERRUPT;
	outw(w, io + ESM_PORT_HOST_IRQ);

	/* Switch to UART mode */
	isis_write_control(chip, 0x3F);
	msleep(10);

	/* Wait for SAM ready signal (0xFE) */
	err = isis_wait_control_bit7(chip, 0);
	if (err < 0)
		return err;

	resp = isis_read_data8(chip);
	if (resp != 0xFE)
		dev_warn(&chip->pci->dev, "Unexpected SAM response: 0x%02x\n",
		         resp);

	/* Get MMT (Memory Management Table) address */
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

	/* Final configuration */
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

	/* Test interrupt generation */
	isis_write_control(chip, 0x48);
	isis_write_data8(chip, 0x00);

	err = isis_wait_control_bit7(chip, 0);
	if (err < 0)
		return err;

	resp = isis_read_data8(chip);
	if (resp != 0x88)
		dev_warn(&chip->pci->dev,
		         "Unexpected interrupt test response: 0x%02x\n", resp);

	/* Unmute output via GPIO */
	outw(0x07FF, io + ESM_GPIO_MASK);
	w = inw(io + ESM_GPIO_DATA) | (1 << 11);
	outw(w, io + ESM_GPIO_DATA);
	outw(0x0FFF, io + ESM_GPIO_MASK);

	dev_info(&chip->pci->dev, "ISIS SAM initialization complete\n");
	return 0;

out_release_fw:
	release_firmware(fw);
	return err;
}

/* ========================================================================
 * Bob Timer Management
 * ======================================================================== */

/**
 * maestro_bob_stop - Stop Bob interrupt timer
 */
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

/**
 * maestro_bob_start - Start Bob timer at current frequency
 */
static void maestro_bob_start(struct maestro *chip)
{
	int prescale, divide;

	/* Calculate timer dividers for target frequency */
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

/**
 * maestro_bob_inc - Increment Bob timer client count
 * @freq: Required frequency for new client
 *
 * Must be called with substream_lock held.
 * Increases frequency if needed to satisfy new client.
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
 * maestro_bob_dec - Decrement Bob timer client count
 *
 * Must be called with substream_lock held.
 * Recalculates optimal frequency or stops timer when no clients remain.
 */
static void maestro_bob_dec(struct maestro *chip)
{
	int clients = atomic_dec_return(&chip->bobclient);

	if (clients <= 0) {
		maestro_bob_stop(chip);
		atomic_set(&chip->bobclient, 0);
	} else if (chip->bob_freq > MAESTRO_BOB_FREQ) {
		struct maestro_pcm_channel *chan;
		int max_freq = MAESTRO_BOB_FREQ;

		/* Find highest required frequency among active streams */
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
 * maestro_calc_bob_rate - Calculate required Bob frequency for stream
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

	freq /= chan->frag_size;

	if (freq < MAESTRO_BOB_FREQ)
		freq = MAESTRO_BOB_FREQ;
	else if (freq > MAESTRO_BOB_FREQ_MAX)
		freq = MAESTRO_BOB_FREQ_MAX;

	return freq;
}

/* ========================================================================
 * APU Programming
 * ======================================================================== */

/**
 * maestro_compute_rate - Convert sample rate to hardware format
 */
static u32 maestro_compute_rate(struct maestro *chip, u32 freq)
{
	return (freq << 16) / chip->clock;
}

/**
 * maestro_apu_set_freq - Set APU playback frequency
 */
static void maestro_apu_set_freq(struct maestro *chip, int apu, int freq)
{
	apu_set_register(chip, apu, 2,
	                 (apu_get_register(chip, apu, 2) & 0x00FF) |
	                 ((freq & 0xFF) << 8) | 0x10);
	apu_set_register(chip, apu, 3, freq >> 8);
}

/**
 * maestro_trigger_apu - Start/stop APU
 * @mode: APU mode (0 = stop, others = various playback modes)
 *
 * Must hold reg_lock.
 */
static inline void maestro_trigger_apu(struct maestro *chip, int apu, int mode)
{
	u16 v = apu_get_register(chip, apu, 0);
	v = (v & 0xFF0F) | (mode << 4);
	apu_set_register(chip, apu, 0, v);
}

/**
 * maestro_program_wavecache - Configure WaveCache channel
 * @channel: Logical channel number (0-3)
 * @addr: Physical DMA address
 * @capture: 1 for capture, 0 for playback
 */
static void maestro_program_wavecache(struct maestro *chip,
                                      struct maestro_pcm_channel *chan,
                                      int channel, u32 addr, int capture)
{
	u32 tmpval = (addr - 0x10) & 0xFFF8;

	if (!capture) {
		if (!(chan->fmt & ESS_FMT_16BIT))
			tmpval |= 4; /* 8-bit flag */
		if (chan->fmt & ESS_FMT_STEREO)
			tmpval |= 2; /* Stereo flag */
	}

	wave_set_register(chip, chan->apu[channel] << 3, tmpval);
}

/**
 * maestro_get_dma_ptr - Read current DMA position
 * Returns position in 16-bit words
 */
static inline unsigned int maestro_get_dma_ptr(struct maestro *chip,
                                                struct maestro_pcm_channel *chan)
{
	unsigned int offset;

	offset = apu_get_register(chip, chan->apu[0], 5);
	offset -= chan->base[0];

	return offset & 0xFFFE; /* Hardware position is in words */
}

/**
 * maestro_pcm_start - Start APU playback/capture
 */
static inline void maestro_pcm_start(struct maestro *chip,
                                     struct maestro_pcm_channel *chan)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);

	/* Reset and start primary APU */
	apu_set_register(chip, chan->apu[0], 5, chan->base[0]);
	maestro_trigger_apu(chip, chan->apu[0], chan->apu_mode[0]);

	/* For capture, start capture APU */
	if (chan->mode == MAESTRO_MODE_CAPTURE) {
		apu_set_register(chip, chan->apu[2], 5, chan->base[2]);
		maestro_trigger_apu(chip, chan->apu[2], chan->apu_mode[2]);
	}

	/* Start secondary APU for stereo */
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

/**
 * maestro_pcm_stop - Stop APU playback/capture
 */
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

/**
 * maestro_playback_setup - Configure APUs for playback
 */
static void maestro_playback_setup(struct maestro *chip,
                                   struct maestro_pcm_channel *chan,
                                   struct snd_pcm_runtime *runtime)
{
	u32 pa, freq;
	int high_apu = 0;
	int channel, apu, i, size;
	unsigned long flags;

	chan->dma_size = snd_pcm_lib_buffer_bytes(chan->substream);
	chan->frag_size = snd_pcm_lib_period_bytes(chan->substream);

	/* Maestro uses 16-bit words internally */
	chan->wav_shift = 1;
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

	spin_lock_irqsave(&chip->reg_lock, flags);

	/* Program each APU channel */
	for (channel = 0; channel <= high_apu; channel++) {
		apu = chan->apu[channel];

		maestro_program_wavecache(chip, chan, channel,
		                          chan->memory->addr, 0);

		/* Convert to WaveCache addressing */
		pa = chan->memory->addr;
		pa -= chip->dma.addr;
		pa >>= 1; /* Convert to words */
		pa |= 0x00400000; /* System RAM flag */

		if (chan->fmt & ESS_FMT_STEREO) {
			if (channel)
				pa |= 0x00800000; /* Right channel flag */
			if (chan->fmt & ESS_FMT_16BIT)
				pa >>= 1;
		}

		chan->base[channel] = pa & 0xFFFF;

		/* Initialize all APU registers */
		for (i = 0; i < 16; i++)
			apu_set_register(chip, apu, i, 0x0000);

		/* Configure APU for playback */
		apu_set_register(chip, apu, 4, ((pa >> 16) & 0xFF) << 8);
		apu_set_register(chip, apu, 5, pa & 0xFFFF);
		apu_set_register(chip, apu, 6, (pa + size) & 0xFFFF);
		apu_set_register(chip, apu, 7, size);
		apu_set_register(chip, apu, 8, 0x0000);
		apu_set_register(chip, apu, 9, 0xD000);
		apu_set_register(chip, apu, 11, 0x0000);
		apu_set_register(chip, apu, 0, 0x400F);

		/* Set APU mode based on format */
		if (chan->fmt & ESS_FMT_16BIT)
			chan->apu_mode[channel] = 0x01; /* 16-bit linear */
		else
			chan->apu_mode[channel] = 0x03; /* 8-bit linear */

		if (chan->fmt & ESS_FMT_STEREO) {
			apu_set_register(chip, apu, 10,
			                 0x8F00 | (channel ? 0 : 0x10));
			chan->apu_mode[channel] += 1; /* Stereo variant */
		} else {
			apu_set_register(chip, apu, 10, 0x8F08);
		}
	}

	/* Enable WaveProcessor interrupts */
	outw(1, chip->io_base + 0x04);
	outw(inw(chip->io_base + ESM_PORT_HOST_IRQ) | ESM_HIRQ_DSIE,
	     chip->io_base + ESM_PORT_HOST_IRQ);

	spin_unlock_irqrestore(&chip->reg_lock, flags);

	/* Set sample rate */
	freq = runtime->rate;
	freq = clamp(freq, 4000U, 48000U);

	if (!(chan->fmt & ESS_FMT_16BIT) && !(chan->fmt & ESS_FMT_STEREO))
		freq >>= 1;

	freq = maestro_compute_rate(chip, freq);

	maestro_apu_set_freq(chip, chan->apu[0], freq);
	maestro_apu_set_freq(chip, chan->apu[1], freq);
}

/* ========================================================================
 * PCM Operations
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

	chan->chip = chip;
	chan->apu[0] = apu1;
	chan->apu[1] = apu1 + 1;
	chan->apu_mode[0] = 0;
	chan->apu_mode[1] = 0;
	chan->running = 0;
	chan->substream = substream;
	chan->mode = MAESTRO_MODE_PLAY;
	chan->memory = NULL;
	spin_lock_init(&chan->lock);
	INIT_LIST_HEAD(&chan->list);

	runtime->private_data = chan;
	
	/* Set hardware capabilities */
	runtime->hw.info = SNDRV_PCM_INFO_MMAP |
	                   SNDRV_PCM_INFO_MMAP_VALID |
	                   SNDRV_PCM_INFO_INTERLEAVED |
	                   SNDRV_PCM_INFO_BLOCK_TRANSFER |
	                   SNDRV_PCM_INFO_RESUME;
	runtime->hw.formats = SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE;
	runtime->hw.rates = SNDRV_PCM_RATE_CONTINUOUS |
	                    SNDRV_PCM_RATE_8000_48000;
	runtime->hw.rate_min = 4000;
	runtime->hw.rate_max = 48000;
	runtime->hw.channels_min = 1;
	runtime->hw.channels_max = 2;
	runtime->hw.buffer_bytes_max = chip->dma.bytes;
	runtime->hw.period_bytes_min = 256;
	runtime->hw.period_bytes_max = chip->dma.bytes;
	runtime->hw.periods_min = 1;
	runtime->hw.periods_max = 1024;
	runtime->hw.fifo_size = 0;

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

	/* Reuse existing allocation if large enough */
	if (chan->memory) {
		if (chan->memory->size >= size) {
			runtime->dma_area = chan->memory->buf;
			runtime->dma_addr = chan->memory->addr;
			runtime->dma_bytes = size;
			return 0;
		}
		maestro_free_memory(chip, chan->memory);
		chan->memory = NULL;
	}

	/* Allocate from DMA pool */
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
	
	runtime->dma_area = NULL;
	runtime->dma_addr = 0;
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
	
	default:
		spin_unlock(&chip->substream_lock);
		return -EINVAL;
	}

	spin_unlock(&chip->substream_lock);
	return 0;
}

static snd_pcm_uframes_t maestro_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct maestro *chip = snd_pcm_substream_chip(substream);
	struct maestro_pcm_channel *chan = substream->runtime->private_data;
	unsigned int ptr;

	if (!chan || !chan->memory)
		return 0;

	ptr = maestro_get_dma_ptr(chip, chan) << chan->wav_shift;
	return bytes_to_frames(substream->runtime, ptr % chan->dma_size);
}

static const struct snd_pcm_ops maestro_playback_ops = {
	.open =        maestro_playback_open,
	.close =       maestro_playback_close,
	.ioctl =       snd_pcm_lib_ioctl,
	.hw_params =   maestro_hw_params,
	.hw_free =     maestro_hw_free,
	.prepare =     maestro_pcm_prepare,
	.trigger =     maestro_pcm_trigger,
	.pointer =     maestro_pcm_pointer,
};

/* ========================================================================
 * Interrupt Handler
 * ======================================================================== */

/**
 * maestro_update_pcm - Update stream position and trigger period elapsed
 */
static void maestro_update_pcm(struct maestro *chip,
                               struct maestro_pcm_channel *chan)
{
	unsigned int hwptr, diff;
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

/**
 * maestro_interrupt - Main interrupt handler
 */
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

	/* Clear WaveProcessor interrupt */
	outw(inw(chip->io_base + 4) & 1, chip->io_base + 4);

	/* Handle hardware volume (optional) */
	if (event & ESM_HWVOL_IRQ) {
		/* TODO: Implement hardware volume control */
	}

	/* Acknowledge all interrupt sources */
	outb(0xFF, chip->io_base + 0x1A);

	/* Update all active streams */
	if (event & ESM_SOUND_IRQ) {
		spin_lock(&chip->substream_lock);
		list_for_each_entry(chan, &chip->substream_list, list) {
			maestro_update_pcm(chip, chan);
		}
		spin_unlock(&chip->substream_lock);
	}

	/* Handle SAM/Dream interrupts if needed */
	if (event & SAM_INTERRUPT) {
		/* TODO: SAM event handling */
	}

	return IRQ_HANDLED;
}

/* ========================================================================
 * Chip Initialization
 * ======================================================================== */

/**
 * maestro_chip_init - Basic chip initialization
 */
static void maestro_chip_init(struct maestro *chip)
{
	u16 w;
	unsigned long iobase = chip->io_base;

	/* Configure legacy audio control */
	w = inw(iobase + ESM_LEGACY_AUDIO_CONTROL);
	w &= ~0x000F;
	w |= 0x0001;
	outw(w, iobase + ESM_LEGACY_AUDIO_CONTROL);

	/* Disable all host interrupts initially */
	w = inw(iobase + ESM_PORT_HOST_IRQ);
	w &= ~(ESM_HIRQ_DSIE | ESM_HIRQ_MPU401 | 
	       ESM_HIRQ_HW_VOLUME | SAM_INTERRUPT);
	outw(w, iobase + ESM_PORT_HOST_IRQ);

	/* Set system clock to 50 MHz */
	chip->clock = 48000;
}

/**
 * maestro_ac97_attach - Initialize AC97 codec
 */
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

/* ========================================================================
 * PCI Driver Interface
 * ======================================================================== */

static int maestro_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
	struct maestro *chip;
	struct snd_card *card;
	struct snd_pcm *pcm;
	int err;
	resource_size_t bar0_start, bar0_len;

	/* Enable PCI device */
	err = pci_enable_device(pdev);
	if (err < 0)
		return err;
	
	pci_set_master(pdev);

	/* Set DMA mask for 28-bit addressing */
	err = dma_set_mask(&pdev->dev, DMA_BIT_MASK(28));
	if (err < 0) {
		dev_err(&pdev->dev, "28-bit DMA not available\n");
		goto err_disable;
	}
	err = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(28));
	if (err < 0) {
		dev_err(&pdev->dev, "28-bit coherent DMA not available\n");
		goto err_disable;
	}

	/* Create ALSA card */
	err = snd_card_new(&pdev->dev, -1, "maestro", THIS_MODULE,
	                   0, &card);
	if (err < 0)
		goto err_disable;

	/* Allocate chip structure */
	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip) {
		err = -ENOMEM;
		goto err_card;
	}

	chip->card = card;
	chip->pci = pdev;
	card->private_data = chip;
	pci_set_drvdata(pdev, chip);

	/* Initialize locks and lists */
	spin_lock_init(&chip->reg_lock);
	spin_lock_init(&chip->substream_lock);
	INIT_LIST_HEAD(&chip->buf_list);
	INIT_LIST_HEAD(&chip->substream_list);
	atomic_set(&chip->bobclient, 0);
	chip->bob_freq = MAESTRO_BOB_FREQ;

	/* Request I/O region */
	bar0_start = pci_resource_start(pdev, 0);
	bar0_len = pci_resource_len(pdev, 0);
	
	if (!request_region(bar0_start, bar0_len, DRIVER_NAME)) {
		dev_err(&pdev->dev, "Cannot reserve I/O region\n");
		err = -EBUSY;
		goto err_free_chip;
	}
	
	chip->io_base = bar0_start;

	/* Request IRQ */
	err = request_irq(pdev->irq, maestro_interrupt, IRQF_SHARED,
	                  DRIVER_NAME, chip);
	if (err < 0) {
		dev_err(&pdev->dev, "Cannot request IRQ %d\n", pdev->irq);
		goto err_region;
	}
	chip->irq = pdev->irq;

	/* Basic chip initialization */
	maestro_chip_init(chip);

	/* Initialize DMA pool (512 KB default) */
	err = maestro_init_dmabuf(chip, 512);
	if (err < 0) {
		dev_err(&pdev->dev, "DMA pool initialization failed: %d\n",
		        err);
		goto err_irq;
	}

	/* Create PCM device */
	err = snd_pcm_new(card, "Maestro PCM", 0, 1, 0, &pcm);
	if (err < 0)
		goto err_dma;
	
	chip->pcm = pcm;
	pcm->private_data = chip;
	strcpy(pcm->name, "Guillemot Maxi Studio ISIS");
	
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
	                &maestro_playback_ops);

	/* Initialize AC97 codec */
	err = maestro_ac97_attach(chip);
	if (err < 0) {
		dev_err(&pdev->dev, "AC97 initialization failed: %d\n", err);
		goto err_dma;
	}

	/* Upload ISIS firmware */
	err = maestro_upload_firmware(chip);
	if (err < 0) {
		dev_err(&pdev->dev, "Firmware upload failed: %d\n", err);
		goto err_dma;
	}

	/* Set card metadata */
	strcpy(card->driver, DRIVER_NAME);
	strcpy(card->shortname, "Guillemot Maxi Studio ISIS");
	snprintf(card->longname, sizeof(card->longname),
	         "%s at 0x%lx, irq %d",
	         card->shortname, chip->io_base, chip->irq);

	/* Register card */
	err = snd_card_register(card);
	if (err < 0)
		goto err_dma;

	dev_info(&pdev->dev,
	         "Guillemot Maxi Studio ISIS initialized successfully\n");
	return 0;

err_dma:
	maestro_free_dmabuf(chip);
err_irq:
	free_irq(chip->irq, chip);
err_region:
	release_region(chip->io_base, pci_resource_len(pdev, 0));
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

	if (!chip)
		return;

	/* Disable interrupts */
	outw(0, chip->io_base + ESM_PORT_HOST_IRQ);

	/* Free resources */
	free_irq(chip->irq, chip);
	maestro_free_dmabuf(chip);
	
	if (chip->card)
		snd_card_free(chip->card);

	release_region(chip->io_base, pci_resource_len(pdev, 0));
	pci_disable_device(pdev);
	
	kfree(chip);
	pci_set_drvdata(pdev, NULL);
}

/* PCI device ID table */
static const struct pci_device_id maestro_ids[] = {
	{ PCI_DEVICE(MAESTRO_VENDOR, MAESTRO_DEVICE) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, maestro_ids);

static struct pci_driver maestro_driver = {
	.name =     DRIVER_NAME,
	.id_table = maestro_ids,
	.probe =    maestro_probe,
	.remove =   maestro_remove,
};

/* ========================================================================
 * Module Init/Exit
 * ======================================================================== */

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

MODULE_AUTHOR("Guillemot Maxi Studio ISIS Driver Team");
MODULE_DESCRIPTION("ALSA driver for Guillemot Maxi Studio ISIS (ESS Maestro-2E)");
MODULE_LICENSE("GPL v2");
MODULE_FIRMWARE("pci64.bin");
