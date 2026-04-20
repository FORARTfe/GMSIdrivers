// SPDX-License-Identifier: GPL-2.0-only
/*
 * hw.h  —  ESS Maestro-2E (ES1978) hardware definitions
 *          Guillemot Maxi Studio ISIS WDM driver
 *
 * All register values hardware-confirmed from:
 *   ES197X.vxd binary analysis (Object 1 @ 0x3BBC0)
 *   maestro.inf v4.05.00.0427
 *   ISIS schematic v0.7 (Jimmy Le Rhun, GPL 2003)
 */

#pragma once

#include <ntddk.h>
#include <wdm.h>
#include <portcls.h>
#include <stdunk.h>

// ─── PCI Identity ─────────────────────────────────────────────────────────
#define ES1978_VENDOR_ID        0x125D
#define ES1978_DEVICE_ID        0x1978
#define ISIS_SUBSYSTEM_VENDOR   0x14AF  // Guillemot Corp.
#define ISIS_SUBSYSTEM_DEVICE   0x0010  // Maxi Studio ISIS

// ─── BAR0 I/O Port Offsets ────────────────────────────────────────────────

// WaveProcessor (WP) indirect register bus
#define REG_WP_DATA             0x00    // WP data port
#define REG_WP_INDEX            0x02    // WP index port

// WaveCache direct ports — NOT through WP bus (BUG-2 confirmation)
#define REG_WC_INDEX            0x10
#define REG_WC_DATA             0x12
#define REG_WC_CONTROL          0x14

// AC97 codec bridge
#define REG_AC97_INDEX          0x30
#define REG_AC97_DATA           0x32

// Ring bus control
#define REG_RING_BUS_L          0x34
#define REG_RING_BUS_H          0x36

// Host interrupt control
#define REG_HOST_IRQ            0x18
#define REG_IRQ_STATUS          0x1A
#define REG_WP_IRQ_ACK          0x04

// GPIO (schematic: amp-mute on GPIO[1], clock-sel on GPIO[4:5])
#define REG_GPIO_DATA           0x60
#define REG_GPIO_MASK           0x64
#define REG_GPIO_DIR            0x68

// Host IRQ bits (REG_HOST_IRQ)
#define HIRQ_DS_IRQ_EN          (1 << 2)    // DirectSound interrupt enable
#define HIRQ_MPU401_IRQ_EN      (1 << 1)
#define HIRQ_HWVOL_IRQ_EN       (1 << 6)    // HW volume — DISABLED on ISIS

// IRQ status bits (REG_IRQ_STATUS)
#define IRQST_SOUND             0x04
#define IRQST_HWVOL             0x40

// ─── WP CRAM Double-Indirection Register Indices ──────────────────────────
// Access APU CRAM via: write address to WP[IDR1], read/write data via WP[IDR0]
#define IDR0_DATA_PORT          0x00
#define IDR1_CRAM_PTR           0x01
#define IDR2_CRAM_DATA          0x02
#define IDR6_TIMER_CTRL         0x06
#define IDR7_WAVE_ROMRAM        0x07

// ─── ES1978 WP Control Register Init Values (from ES197X.vxd @ 0x3BBC0) ───
// These differ from generic maestro.c ESS1968 values — ES1978-specific.
#define WP_R07_INIT             0x1540  // IDR7: system RAM + ES1978 mode bits
#define WP_R08_INIT             0xB723  // WP mixer L/R (was 0xB004 on ES1968)
#define WP_R09_INIT             0x001B  // WP ctrl (same on both)
#define WP_R0A_INIT             0x5F1F  // WP filter (was 0x8000)
#define WP_R0B_INIT             0xDF9F  // WP reverb (was 0x3F37)
#define WP_R0C_INIT             0x1377  // WP routing (was 0x0098)
#define WP_R0D_INIT             0x7632  // WP gain (same on both)
#define WP_R0E_INIT             0x0D16  // WP ext (ES1978-only)

// ─── Clock Reference (BUG-3 confirmed) ────────────────────────────────────
// 50 MHz oscillator / 1024 internal prescaler = 48828 Hz
// Used in APU sample-rate register computation.
#define MAESTRO_CLOCK_HZ        (50000000L / 1024L)  // 48828

// ─── Bob Timer ────────────────────────────────────────────────────────────
#define ESS_SYSCLK              50000000UL
#define BOB_FREQ_DEFAULT        200     // Hz
#define BOB_FREQ_MAX            800     // Hz

// ─── APU Engine ───────────────────────────────────────────────────────────
#define NR_APUS                 64
#define NR_APU_REGS             16

// APU mode nibble values (unshifted; shifted <<4 into APU reg[0] bits[7:4])
#define APU_MODE_16BIT_MONO     0x1
#define APU_MODE_16BIT_STEREO   0x2
#define APU_MODE_8BIT_MONO      0x3
#define APU_MODE_8BIT_STEREO    0x4
#define APU_MODE_SRC            0xB     // Sample Rate Converter (capture)
#define APU_MODE_INPUTMIXER     0x9     // Input Mixer (capture, ADC→mixbuf)

// ─── DMA Pool ─────────────────────────────────────────────────────────────
#define DMA_POOL_SIZE_BYTES     (512 * 1024)    // 512 KB
#define DMA_POOL_RSVD           512             // hardware status FIFO
#define DMA_ALIGN               64              // WaveCache granularity
#define MIXBUF_SIZE             512             // capture intermediate buffer

// ─── GPIO / Amp Control (from maestro.inf TurnOnOffExtAmp=1) ──────────────
// Method 1 = Maestro GPIO pin 1 (bit 1) controls ext amp (PCM1718E mute)
// GPIOVal_BM = GPIOVal_NOBM = 0x09 = bits 0 and 3 for amp-on state
#define GPIO_AMP_BIT            (1 << 1)        // GPIO bit 1 = TurnOnOffExtAmp
#define GPIO_VAL_AMP_ON         0x09            // bits 0,3 = high when amp on

// ─── AC97 Registers ───────────────────────────────────────────────────────
#define AC97_RESET              0x00
#define AC97_MASTER_VOL         0x02
#define AC97_HEADPHONE_VOL      0x04
#define AC97_MASTER_MONO        0x06
#define AC97_MASTER_TONE        0x08
#define AC97_PC_BEEP            0x0A
#define AC97_PHONE_VOL          0x0C
#define AC97_MIC_VOL            0x0E
#define AC97_LINE_VOL           0x10
#define AC97_CD_VOL             0x12
#define AC97_VIDEO_VOL          0x14
#define AC97_AUX_VOL            0x16
#define AC97_PCM_OUT_VOL        0x18
#define AC97_REC_SELECT         0x1A
#define AC97_REC_GAIN           0x1C
#define AC97_GENERAL_PURPOSE    0x20
#define AC97_3D_CTRL            0x22
#define AC97_POWERDOWN          0x26
#define AC97_VENDOR_ID1         0x7C
#define AC97_VENDOR_ID2         0x7E

#define AC97_MUTE               (1 << 15)
#define AC97_PR3_ANALOG_PWRDN   (1 << 2)   // keep clear: DisablePR3State=1

// ─── Supported Formats ────────────────────────────────────────────────────
#define FMT_STEREO              0x01
#define FMT_16BIT               0x02

// ─── Forward Declarations ─────────────────────────────────────────────────
class CMaestroHW;

// ─── APU Stream Descriptor ────────────────────────────────────────────────
struct STREAM_DESC {
    ULONG       dma_size;       // buffer bytes
    ULONG       frag_size;      // period bytes
    ULONG       wav_shift;      // byte↔word shift
    UCHAR       fmt;            // FMT_* flags
    UCHAR       apu[4];         // APU indices (0/1=play; 0/1=SRC, 2/3=InMix)
    UCHAR       apu_mode[4];
    USHORT      base[4];        // APU start-ptr cache
    ULONG       hwptr;
    PHYSICAL_ADDRESS dma_phys;  // buffer bus address
    PHYSICAL_ADDRESS mixbuf_phys;
    PVOID       dma_virt;
    PVOID       mixbuf_virt;
    ULONG       mixbuf_size;
    BOOLEAN     running;
    BOOLEAN     is_capture;
    int         bob_freq;
};

// ─── CHardware — hardware abstraction ─────────────────────────────────────
class CHardware
{
    LONG            m_ref;
    PULONG          m_iobase;       // mapped BAR0 (I/O port base)
    ULONG           m_iobase_raw;
    KSPIN_LOCK      m_reg_lock;
    KSPIN_LOCK      m_substream_lock;
    LONG            m_bob_clients;
    ULONG           m_bob_freq;
    ULONG           m_clock;        // 48828 Hz

    // APU allocation bitmap
    BOOLEAN         m_apu[NR_APUS];

    // DMA pool
    PHYSICAL_ADDRESS m_pool_phys;
    PVOID            m_pool_virt;
    ULONG            m_pool_size;

    // Active streams (simple fixed array — max 2 simultaneous)
    STREAM_DESC*    m_streams[4];
    ULONG           m_stream_count;

    PDEVICE_OBJECT  m_pdo;

public:
    CHardware();
    ~CHardware();

    NTSTATUS    Initialize(PULONG iobase, PDEVICE_OBJECT pdo);
    void        Shutdown();

    // ── WP / WC register access ───────────────────────────────────────
    USHORT      WpRead(USHORT reg);
    void        WpWrite(USHORT reg, USHORT val);
    void        WcWrite(USHORT reg, USHORT val);
    USHORT      WcRead(USHORT reg);

    // ── APU CRAM access (unlocked — call with reg_lock held) ──────────
    void        ApuSet(UCHAR apu, UCHAR reg, USHORT data);
    USHORT      ApuGet(UCHAR apu, UCHAR reg);

    // ── AC97 ─────────────────────────────────────────────────────────
    USHORT      Ac97Read(USHORT reg);
    void        Ac97Write(USHORT reg, USHORT val);

    // ── DMA pool ─────────────────────────────────────────────────────
    NTSTATUS    AllocDma(ULONG size, PVOID* virt, PHYSICAL_ADDRESS* phys);
    void        FreeDma(PVOID virt, ULONG size);

    // ── APU allocation ────────────────────────────────────────────────
    int         AllocApuPair();
    int         AllocApuQuad();
    void        FreeApus(int base, int count);

    // ── Bob timer ─────────────────────────────────────────────────────
    void        BobStart();
    void        BobStop();
    void        BobIncClient(ULONG freq);
    void        BobDecClient();

    // ── Stream setup ─────────────────────────────────────────────────
    NTSTATUS    SetupPlayback(STREAM_DESC* sd, ULONG rate, ULONG channels,
                              ULONG bits_per_sample);
    NTSTATUS    SetupCapture(STREAM_DESC* sd, ULONG rate, ULONG channels,
                             ULONG bits_per_sample);
    void        StartStream(STREAM_DESC* sd);
    void        StopStream(STREAM_DESC* sd);
    ULONG       GetStreamPosition(STREAM_DESC* sd); // bytes

    // ── Interrupt handler ─────────────────────────────────────────────
    void        InterruptDpc();
    void        RegisterStream(STREAM_DESC* sd);
    void        UnregisterStream(STREAM_DESC* sd);

    // ── Misc ─────────────────────────────────────────────────────────
    ULONG       GetClock() const { return m_clock; }

private:
    void        ChipInit();
    void        Ac97Reset();
    void        Ac97HardwareInit();
    void        SetWcBase();
    void        ProgramWavecache(STREAM_DESC* sd, int slot,
                                 ULONG addr, BOOLEAN capture);
    void        TriggerApu(UCHAR apu, int mode);
    void        SetApuFreq(UCHAR apu, ULONG freq);
    ULONG       ComputeRate(ULONG sample_rate);
    ULONG       CalcBobFreq(STREAM_DESC* sd, ULONG rate);
    void        SoundReset();
    inline USHORT IoRead16(ULONG offset);
    inline void   IoWrite16(ULONG offset, USHORT val);
    inline UCHAR  IoRead8(ULONG offset);
    inline void   IoWrite8(ULONG offset, UCHAR val);
};
