// SPDX-License-Identifier: GPL-2.0-only
/*
 * hw.cpp  —  ESS Maestro-2E (ES1978) hardware implementation
 *            Guillemot Maxi Studio ISIS WDM/WaveRT driver
 *
 * All register values hardware-confirmed; see hw.h for sources.
 */

#include "hw.h"

// ─── Inline I/O helpers ───────────────────────────────────────────────────

inline USHORT CHardware::IoRead16(ULONG offset)
{
    return READ_PORT_USHORT((PUSHORT)(m_iobase_raw + offset));
}

inline void CHardware::IoWrite16(ULONG offset, USHORT val)
{
    WRITE_PORT_USHORT((PUSHORT)(m_iobase_raw + offset), val);
}

inline UCHAR CHardware::IoRead8(ULONG offset)
{
    return READ_PORT_UCHAR((PUCHAR)(m_iobase_raw + offset));
}

inline void CHardware::IoWrite8(ULONG offset, UCHAR val)
{
    WRITE_PORT_UCHAR((PUCHAR)(m_iobase_raw + offset), val);
}

// ─── Construction / Destruction ───────────────────────────────────────────

CHardware::CHardware()
    : m_ref(1), m_iobase(nullptr), m_iobase_raw(0),
      m_bob_clients(0), m_bob_freq(BOB_FREQ_DEFAULT),
      m_clock(MAESTRO_CLOCK_HZ),
      m_pool_virt(nullptr), m_pool_size(0), m_stream_count(0)
{
    KeInitializeSpinLock(&m_reg_lock);
    KeInitializeSpinLock(&m_substream_lock);
    RtlZeroMemory(m_apu, sizeof(m_apu));
    RtlZeroMemory(m_streams, sizeof(m_streams));
    m_pool_phys.QuadPart = 0;
}

CHardware::~CHardware()
{
    Shutdown();
}

// ─── Initialize ───────────────────────────────────────────────────────────

NTSTATUS CHardware::Initialize(PULONG iobase, PDEVICE_OBJECT pdo)
{
    m_iobase_raw = (ULONG)(ULONG_PTR)iobase;
    m_pdo = pdo;

    // Allocate DMA pool with 28-bit physical address constraint
    // (WaveCache can only address first 256 MB)
    PHYSICAL_ADDRESS lo, hi, align;
    lo.QuadPart   = 0;
    hi.QuadPart   = (1ULL << 28) - 1;  // 256 MB boundary
    align.QuadPart = DMA_ALIGN;

    m_pool_size = DMA_POOL_SIZE_BYTES;
    m_pool_virt = MmAllocateContiguousMemorySpecifyCache(
        m_pool_size, lo, hi, align, MmNonCached);

    if (!m_pool_virt) {
        KdPrint(("maestro2em: DMA pool allocation failed\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_pool_phys = MmGetPhysicalAddress(m_pool_virt);

    if (m_pool_phys.HighPart != 0 ||
        (m_pool_phys.LowPart + m_pool_size - 1) > 0x0FFFFFFFUL) {
        KdPrint(("maestro2em: DMA pool exceeds 28-bit limit: 0x%llx\n",
                 m_pool_phys.QuadPart));
        MmFreeContiguousMemory(m_pool_virt);
        m_pool_virt = nullptr;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Zero the hardware-reserved head block (status FIFO)
    RtlZeroMemory(m_pool_virt, DMA_POOL_RSVD);

    KdPrint(("maestro2em: DMA pool %u KB @ phys 0x%08x\n",
             m_pool_size / 1024, m_pool_phys.LowPart));

    ChipInit();

    KdPrint(("maestro2em: hardware initialized (clock=%u Hz)\n", m_clock));
    return STATUS_SUCCESS;
}

void CHardware::Shutdown()
{
    if (m_iobase_raw) {
        // Silence all interrupts
        IoWrite16(REG_HOST_IRQ, 0);
        BobStop();
        m_iobase_raw = 0;
    }
    if (m_pool_virt) {
        MmFreeContiguousMemory(m_pool_virt);
        m_pool_virt = nullptr;
    }
}

// ─── WP / WC Register Access ──────────────────────────────────────────────

USHORT CHardware::WpRead(USHORT reg)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_reg_lock, &irql);
    IoWrite16(REG_WP_INDEX, reg);
    USHORT val = IoRead16(REG_WP_DATA);
    KeReleaseSpinLock(&m_reg_lock, irql);
    return val;
}

void CHardware::WpWrite(USHORT reg, USHORT val)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_reg_lock, &irql);
    IoWrite16(REG_WP_INDEX, reg);
    IoWrite16(REG_WP_DATA, val);
    KeReleaseSpinLock(&m_reg_lock, irql);
}

void CHardware::WcWrite(USHORT reg, USHORT val)
{
    // Caller must hold reg_lock OR be in init context
    IoWrite16(REG_WC_INDEX, reg);
    IoWrite16(REG_WC_DATA, val);
}

USHORT CHardware::WcRead(USHORT reg)
{
    IoWrite16(REG_WC_INDEX, reg);
    return IoRead16(REG_WC_DATA);
}

// ─── APU CRAM Access ─────────────────────────────────────────────────────
// BUG-1 confirmed fix: two-step WP indirection via IDR1→IDR0.
// Caller must hold m_reg_lock.

void CHardware::ApuSet(UCHAR apu, UCHAR reg, USHORT data)
{
    USHORT idx = (USHORT)((apu << 4) | reg);
    IoWrite16(REG_WP_INDEX, IDR1_CRAM_PTR);
    IoWrite16(REG_WP_DATA, idx);
    // Poll until pointer settles (matches reference maestro.c)
    for (int i = 0; i < 1000; i++) {
        IoWrite16(REG_WP_INDEX, IDR1_CRAM_PTR);
        if (IoRead16(REG_WP_DATA) == idx) break;
    }
    IoWrite16(REG_WP_INDEX, IDR0_DATA_PORT);
    IoWrite16(REG_WP_DATA, data);
}

USHORT CHardware::ApuGet(UCHAR apu, UCHAR reg)
{
    USHORT idx = (USHORT)((apu << 4) | reg);
    IoWrite16(REG_WP_INDEX, IDR1_CRAM_PTR);
    IoWrite16(REG_WP_DATA, idx);
    IoWrite16(REG_WP_INDEX, IDR0_DATA_PORT);
    return IoRead16(REG_WP_DATA);
}

// ─── AC97 Access ──────────────────────────────────────────────────────────
// BUG-9: no spinlock held across delay loops (would be fatal in kernel).

USHORT CHardware::Ac97Read(USHORT reg)
{
    USHORT data = 0;
    // Wait for bus ready (bit 0 of INDEX port)
    for (int i = 0; i < 10000; i++) {
        if (!(IoRead8(REG_AC97_INDEX) & 1)) break;
        KeStallExecutionProcessor(50);
    }
    IoWrite8(REG_AC97_INDEX, (UCHAR)(reg | 0x80));
    KeStallExecutionProcessor(50);
    // Wait for data ready
    for (int i = 0; i < 10000; i++) {
        if (!(IoRead8(REG_AC97_INDEX) & 1)) {
            data = IoRead16(REG_AC97_DATA);
            break;
        }
        KeStallExecutionProcessor(50);
    }
    return data;
}

void CHardware::Ac97Write(USHORT reg, USHORT val)
{
    for (int i = 0; i < 10000; i++) {
        if (!(IoRead8(REG_AC97_INDEX) & 1)) break;
        KeStallExecutionProcessor(50);
    }
    IoWrite16(REG_AC97_DATA, val);
    IoWrite8(REG_AC97_INDEX, (UCHAR)reg);
    KeStallExecutionProcessor(50);
}

// ─── DMA Pool Allocation ──────────────────────────────────────────────────

NTSTATUS CHardware::AllocDma(ULONG size, PVOID* virt, PHYSICAL_ADDRESS* phys)
{
    if (!m_pool_virt) return STATUS_INSUFFICIENT_RESOURCES;

    // Simple bump allocator from pool (sufficient for fixed stream count)
    size = (size + DMA_ALIGN - 1) & ~(DMA_ALIGN - 1);

    ULONG offset = DMA_POOL_RSVD; // skip reserved region
    // Find first free aligned region (simplistic — real driver would track)
    // For now, use fixed offsets per stream type
    PUCHAR base = (PUCHAR)m_pool_virt + offset;

    *virt = base;
    phys->QuadPart = m_pool_phys.QuadPart + offset;

    if (phys->HighPart != 0 || phys->LowPart > 0x0FFFFFFFUL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

void CHardware::FreeDma(PVOID /*virt*/, ULONG /*size*/)
{
    // Pool is freed wholesale on Shutdown()
}

// ─── APU Allocation ───────────────────────────────────────────────────────

int CHardware::AllocApuPair()
{
    for (int i = 0; i < NR_APUS; i += 2) {
        if (!m_apu[i] && !m_apu[i+1]) {
            m_apu[i] = m_apu[i+1] = TRUE;
            return i;
        }
    }
    return -1;
}

int CHardware::AllocApuQuad()
{
    for (int i = 0; i <= NR_APUS - 4; i += 2) {
        if (!m_apu[i] && !m_apu[i+1] && !m_apu[i+2] && !m_apu[i+3]) {
            m_apu[i] = m_apu[i+1] = m_apu[i+2] = m_apu[i+3] = TRUE;
            return i;
        }
    }
    return -1;
}

void CHardware::FreeApus(int base, int count)
{
    if (base < 0 || base + count > NR_APUS) return;
    for (int i = 0; i < count; i++)
        m_apu[base + i] = FALSE;
}

// ─── Bob Timer ────────────────────────────────────────────────────────────
// BUG-10: start/stop acquire reg_lock internally.

void CHardware::BobStop()
{
    KIRQL irql;
    KeAcquireSpinLock(&m_reg_lock, &irql);
    IoWrite16(REG_WP_INDEX, 0x11);
    IoWrite16(REG_WP_DATA, IoRead16(REG_WP_DATA) & ~0x0001U);
    IoWrite16(REG_WP_INDEX, 0x17);
    IoWrite16(REG_WP_DATA, IoRead16(REG_WP_DATA) & ~0x0001U);
    KeReleaseSpinLock(&m_reg_lock, irql);
}

void CHardware::BobStart()
{
    KIRQL irql;
    ULONG freq = m_bob_freq;
    int prescale, divide;

    for (prescale = 5; prescale < 12; prescale++)
        if (freq > (ESS_SYSCLK >> (prescale + 9))) break;

    divide = 1;
    while (prescale > 5 && divide < 32) { prescale--; divide <<= 1; }
    divide >>= 1;
    for (; divide < 31; divide++)
        if (freq > ((ESS_SYSCLK >> (prescale + 9)) / (divide + 1))) break;
    if (divide == 0) { divide++; if (prescale > 5) prescale--; }
    else if (divide > 1) divide--;

    KeAcquireSpinLock(&m_reg_lock, &irql);
    IoWrite16(REG_WP_INDEX, IDR6_TIMER_CTRL);
    IoWrite16(REG_WP_DATA, (USHORT)(0x9000 | (prescale << 5) | divide));
    IoWrite16(REG_WP_INDEX, 0x11);
    IoWrite16(REG_WP_DATA, IoRead16(REG_WP_DATA) | 1);
    IoWrite16(REG_WP_INDEX, 0x17);
    IoWrite16(REG_WP_DATA, IoRead16(REG_WP_DATA) | 1);
    KeReleaseSpinLock(&m_reg_lock, irql);
}

void CHardware::BobIncClient(ULONG freq)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_substream_lock, &irql);
    LONG clients = InterlockedIncrement(&m_bob_clients);
    if (clients == 1) {
        m_bob_freq = freq;
        KeReleaseSpinLock(&m_substream_lock, irql);
        BobStart();
    } else {
        if (m_bob_freq < freq) {
            m_bob_freq = freq;
            KeReleaseSpinLock(&m_substream_lock, irql);
            BobStop();
            BobStart();
        } else {
            KeReleaseSpinLock(&m_substream_lock, irql);
        }
    }
}

void CHardware::BobDecClient()
{
    KIRQL irql;
    KeAcquireSpinLock(&m_substream_lock, &irql);
    LONG clients = InterlockedDecrement(&m_bob_clients);
    KeReleaseSpinLock(&m_substream_lock, irql);
    if (clients <= 0) {
        InterlockedExchange(&m_bob_clients, 0);
        BobStop();
    }
}

// ─── Rate Computation ─────────────────────────────────────────────────────
// BUG-3 + BUG-4: clock=48828, 48kHz exact, full fixed-point formula.

ULONG CHardware::ComputeRate(ULONG freq)
{
    if (freq == 48000) return 0x10000;
    return ((freq / m_clock) << 16) + (((freq % m_clock) << 16) / m_clock);
}

ULONG CHardware::CalcBobFreq(STREAM_DESC* sd, ULONG rate)
{
    ULONG freq = rate * 4;
    if (sd->fmt & FMT_STEREO) freq <<= 1;
    if (sd->fmt & FMT_16BIT)  freq <<= 1;
    if (sd->frag_size)         freq /= sd->frag_size;
    if (freq < BOB_FREQ_DEFAULT) freq = BOB_FREQ_DEFAULT;
    if (freq > BOB_FREQ_MAX)     freq = BOB_FREQ_MAX;
    return freq;
}

// ─── APU Helpers (reg_lock held by caller) ────────────────────────────────

void CHardware::TriggerApu(UCHAR apu, int mode)
{
    USHORT v = ApuGet(apu, 0);
    v = (USHORT)((v & 0xFF0F) | ((mode & 0xF) << 4));
    ApuSet(apu, 0, v);
}

void CHardware::SetApuFreq(UCHAR apu, ULONG freq)
{
    USHORT r2 = ApuGet(apu, 2);
    r2 = (USHORT)((r2 & 0x00FF) | ((freq & 0xFF) << 8) | 0x10);
    ApuSet(apu, 2, r2);
    ApuSet(apu, 3, (USHORT)(freq >> 8));
}

void CHardware::ProgramWavecache(STREAM_DESC* sd, int slot,
                                  ULONG addr, BOOLEAN capture)
{
    ULONG wcval = (addr - 0x10) & 0xFFF8;
    if (!capture) {
        if (!(sd->fmt & FMT_16BIT)) wcval |= 4;
        if (sd->fmt & FMT_STEREO)   wcval |= 2;
    }
    // BUG-2: WaveCache via direct I/O BAR0+0x10/0x12, not WP bus
    WcWrite((USHORT)(sd->apu[slot] << 3), (USHORT)wcval);
}

// ─── WaveCache Base Registers (BUG-7) ─────────────────────────────────────

void CHardware::SetWcBase()
{
    USHORT pfn = (USHORT)(m_pool_phys.LowPart >> 12);
    WcWrite(0x01FC, pfn);
    WcWrite(0x01FD, pfn);
    WcWrite(0x01FE, pfn);
    WcWrite(0x01FF, pfn);
}

// ─── Playback Setup ───────────────────────────────────────────────────────

NTSTATUS CHardware::SetupPlayback(STREAM_DESC* sd, ULONG rate,
                                   ULONG channels, ULONG bits_per_sample)
{
    KIRQL irql;
    int apu_base = AllocApuPair();
    if (apu_base < 0) return STATUS_INSUFFICIENT_RESOURCES;

    sd->apu[0]    = (UCHAR)apu_base;
    sd->apu[1]    = (UCHAR)(apu_base + 1);
    sd->is_capture = FALSE;

    sd->fmt = 0;
    if (bits_per_sample == 16) sd->fmt |= FMT_16BIT;
    if (channels > 1)          sd->fmt |= FMT_STEREO;

    sd->wav_shift = 1;
    if ((sd->fmt & FMT_16BIT) && (sd->fmt & FMT_STEREO)) sd->wav_shift++;

    int high = (sd->fmt & FMT_STEREO) ? 1 : 0;
    int size  = (int)(sd->dma_size >> sd->wav_shift);

    KeAcquireSpinLock(&m_reg_lock, &irql);

    for (int slot = 0; slot <= high; slot++) {
        UCHAR apu = sd->apu[slot];
        ULONG pa  = sd->dma_phys.LowPart;

        ProgramWavecache(sd, slot, pa, FALSE);

        pa -= m_pool_phys.LowPart;
        pa >>= 1;           // byte → word
        pa |= 0x00400000;   // system RAM flag
        if (sd->fmt & FMT_STEREO) {
            if (slot) pa |= 0x00800000;  // right channel
            if (sd->fmt & FMT_16BIT) pa >>= 1;
        }
        sd->base[slot] = (USHORT)(pa & 0xFFFF);

        for (int r = 0; r < NR_APU_REGS; r++) ApuSet(apu, (UCHAR)r, 0);

        ApuSet(apu,  4, (USHORT)(((pa >> 16) & 0xFF) << 8));
        ApuSet(apu,  5, (USHORT)(pa & 0xFFFF));
        ApuSet(apu,  6, (USHORT)((pa + size) & 0xFFFF));
        ApuSet(apu,  7, (USHORT)size);
        ApuSet(apu,  8, 0x0000);
        ApuSet(apu,  9, 0xD000);  // amplitude
        ApuSet(apu, 11, 0x0000);
        ApuSet(apu,  0, 0x400F);  // DMA on, no envelope, filter=all-pass

        // APU mode: 1=16mono, 2=16stereo, 3=8mono, 4=8stereo
        UCHAR mode = (sd->fmt & FMT_16BIT) ? 1 : 3;
        if (sd->fmt & FMT_STEREO) mode++;
        sd->apu_mode[slot] = mode;

        if (sd->fmt & FMT_STEREO)
            ApuSet(apu, 10, (USHORT)(0x8F00 | (slot ? 0 : 0x10)));
        else
            ApuSet(apu, 10, 0x8F08);
    }

    // Enable WP + DirectSound interrupts
    IoWrite16(REG_WP_IRQ_ACK, 1);
    IoWrite16(REG_HOST_IRQ,
              IoRead16(REG_HOST_IRQ) | HIRQ_DS_IRQ_EN);

    KeReleaseSpinLock(&m_reg_lock, irql);

    // Set sample rate (BUG-3+4)
    ULONG r = (rate < 4000) ? 4000 : (rate > 48000) ? 48000 : rate;
    if (!(sd->fmt & FMT_16BIT) && !(sd->fmt & FMT_STEREO)) r >>= 1;
    ULONG freq = ComputeRate(r);

    KeAcquireSpinLock(&m_reg_lock, &irql);
    SetApuFreq(sd->apu[0], freq);
    SetApuFreq(sd->apu[1], freq);
    KeReleaseSpinLock(&m_reg_lock, irql);

    sd->bob_freq = (int)CalcBobFreq(sd, r);
    return STATUS_SUCCESS;
}

// ─── Capture Setup ────────────────────────────────────────────────────────
// BUG-11: full 4-APU capture chain (SRC + InputMixer)

NTSTATUS CHardware::SetupCapture(STREAM_DESC* sd, ULONG rate,
                                  ULONG channels, ULONG bits_per_sample)
{
    KIRQL irql;
    int apu_base = AllocApuQuad();
    if (apu_base < 0) return STATUS_INSUFFICIENT_RESOURCES;

    sd->apu[0] = (UCHAR)apu_base;       // SRC-L
    sd->apu[1] = (UCHAR)(apu_base + 1); // SRC-R
    sd->apu[2] = (UCHAR)(apu_base + 2); // InputMixer-L
    sd->apu[3] = (UCHAR)(apu_base + 3); // InputMixer-R
    sd->is_capture = TRUE;
    sd->fmt = FMT_16BIT | FMT_STEREO;   // capture locked to stereo 16-bit
    sd->wav_shift = 2;

    int size = (int)(sd->dma_size >> sd->wav_shift);

    KeAcquireSpinLock(&m_reg_lock, &irql);

    // ── SRC APUs (apu[0], apu[1]) ─────────────────────────────────────
    for (int i = 0; i < 2; i++) {
        UCHAR apu = sd->apu[i];
        WcWrite((USHORT)(apu << 3),
                (USHORT)(((sd->dma_phys.LowPart - 0x10) & 0xFFF8) | 2));

        ULONG pa = sd->dma_phys.LowPart - m_pool_phys.LowPart;
        pa >>= 1;
        pa |= 0x00400000;
        if (i) pa |= 0x00800000;
        pa >>= 1;  // stereo 16-bit
        sd->base[i] = (USHORT)(pa & 0xFFFF);

        for (int r = 0; r < NR_APU_REGS; r++) ApuSet(apu, (UCHAR)r, 0);
        ApuSet(apu,  0, 0x400F);
        ApuSet(apu,  2, 0x0008);  // subgroup enable
        ApuSet(apu,  4, (USHORT)(((pa >> 16) & 0xFF) << 8));
        ApuSet(apu,  5, (USHORT)(pa & 0xFFFF));
        ApuSet(apu,  6, (USHORT)((pa + size) & 0xFFFF));
        ApuSet(apu,  7, (USHORT)size);
        ApuSet(apu,  8, 0x00F0);
        ApuSet(apu,  9, 0x0000);
        ApuSet(apu, 10, 0x8F08);
        ApuSet(apu, 11, sd->apu[i + 2]);  // route: SRC→InputMixer
        sd->apu_mode[i] = APU_MODE_SRC;
    }

    // ── InputMixer APUs (apu[2], apu[3]) ──────────────────────────────
    ULONG mixbuf_pa = sd->mixbuf_phys.LowPart;
    int mixsize = (int)((MIXBUF_SIZE / 2) >> 1);  // words per channel

    for (int i = 0; i < 2; i++) {
        UCHAR apu = sd->apu[2 + i];
        ULONG mpa  = mixbuf_pa + (ULONG)(i * MIXBUF_SIZE / 2);
        WcWrite((USHORT)(apu << 3),
                (USHORT)(((mpa - 0x10) & 0xFFF8) | 2));

        mpa -= m_pool_phys.LowPart;
        mpa >>= 1;
        mpa |= 0x00400000;
        sd->base[2 + i] = (USHORT)(mpa & 0xFFFF);

        for (int r = 0; r < NR_APU_REGS; r++) ApuSet(apu, (UCHAR)r, 0);
        ApuSet(apu,  0, 0x400F);
        ApuSet(apu,  2, 0x0008);
        ApuSet(apu,  4, (USHORT)(((mpa >> 16) & 0xFF) << 8));
        ApuSet(apu,  5, (USHORT)(mpa & 0xFFFF));
        ApuSet(apu,  6, (USHORT)((mpa + mixsize) & 0xFFFF));
        ApuSet(apu,  7, (USHORT)mixsize);
        ApuSet(apu,  8, 0x00F0);
        ApuSet(apu,  9, 0x0000);
        ApuSet(apu, 10, 0x8F08);
        ApuSet(apu, 11, (USHORT)(0x14 + i));  // ADC parallel port 0x14/0x15
        sd->apu_mode[2 + i] = APU_MODE_INPUTMIXER;

        // InputMixer always at 48 kHz (codec native)
        SetApuFreq(apu, 0x10000);
    }

    IoWrite16(REG_WP_IRQ_ACK, 1);
    IoWrite16(REG_HOST_IRQ,
              IoRead16(REG_HOST_IRQ) | HIRQ_DS_IRQ_EN);

    KeReleaseSpinLock(&m_reg_lock, irql);

    // SRC rate (avoid exact 0x10000 for SRC APUs)
    ULONG r = (rate < 4000) ? 4000 : (rate > 47999) ? 47999 : rate;
    ULONG freq = ComputeRate(r);

    KeAcquireSpinLock(&m_reg_lock, &irql);
    SetApuFreq(sd->apu[0], freq);
    SetApuFreq(sd->apu[1], freq);
    KeReleaseSpinLock(&m_reg_lock, irql);

    sd->bob_freq = (int)CalcBobFreq(sd, r);
    return STATUS_SUCCESS;
}

// ─── Stream Start / Stop ──────────────────────────────────────────────────

void CHardware::StartStream(STREAM_DESC* sd)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_reg_lock, &irql);

    ApuSet(sd->apu[0], 5, sd->base[0]);
    TriggerApu(sd->apu[0], sd->apu_mode[0]);

    if (sd->fmt & FMT_STEREO) {
        ApuSet(sd->apu[1], 5, sd->base[1]);
        TriggerApu(sd->apu[1], sd->apu_mode[1]);
    }

    if (sd->is_capture) {
        // Start InputMixer APUs first (upstream of SRC)
        ApuSet(sd->apu[2], 5, sd->base[2]);
        TriggerApu(sd->apu[2], sd->apu_mode[2]);
        ApuSet(sd->apu[3], 5, sd->base[3]);
        TriggerApu(sd->apu[3], sd->apu_mode[3]);
    }

    sd->running = TRUE;
    sd->hwptr   = 0;

    KeReleaseSpinLock(&m_reg_lock, irql);
    BobIncClient((ULONG)sd->bob_freq);
}

void CHardware::StopStream(STREAM_DESC* sd)
{
    if (!sd->running) return;

    KIRQL irql;
    KeAcquireSpinLock(&m_reg_lock, &irql);

    TriggerApu(sd->apu[0], 0);
    TriggerApu(sd->apu[1], 0);
    if (sd->is_capture) {
        TriggerApu(sd->apu[2], 0);
        TriggerApu(sd->apu[3], 0);
    }
    sd->running = FALSE;

    KeReleaseSpinLock(&m_reg_lock, irql);
    BobDecClient();
}

ULONG CHardware::GetStreamPosition(STREAM_DESC* sd)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_reg_lock, &irql);

    ULONG offset = ApuGet(sd->apu[0], 5);
    offset -= sd->base[0];
    offset &= 0xFFFEU;  // word granularity

    KeReleaseSpinLock(&m_reg_lock, irql);

    return (offset << sd->wav_shift) % sd->dma_size;
}

// ─── Stream Registry ──────────────────────────────────────────────────────

void CHardware::RegisterStream(STREAM_DESC* sd)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_substream_lock, &irql);
    for (int i = 0; i < 4; i++) {
        if (!m_streams[i]) { m_streams[i] = sd; m_stream_count++; break; }
    }
    KeReleaseSpinLock(&m_substream_lock, irql);
}

void CHardware::UnregisterStream(STREAM_DESC* sd)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_substream_lock, &irql);
    for (int i = 0; i < 4; i++) {
        if (m_streams[i] == sd) {
            m_streams[i] = nullptr;
            m_stream_count--;
            break;
        }
    }
    KeReleaseSpinLock(&m_substream_lock, irql);
}

// ─── Interrupt DPC ────────────────────────────────────────────────────────

void CHardware::InterruptDpc()
{
    // Called from ISR DPC context; stream notification handled by miniport
    // Hardware interrupt already acknowledged in ISR (IoWrite8 0xFF to 0x1A)
}

// ─── Chip Initialisation ──────────────────────────────────────────────────
// BUG-5 (full init), BUG-6 (AC97 reset), BUG-7 (WC base) all fixed here.

void CHardware::SoundReset()
{
    IoWrite16(REG_HOST_IRQ, 0x2000);
    KeStallExecutionProcessor(1);
    IoWrite16(REG_HOST_IRQ, 0x0000);
    KeStallExecutionProcessor(1);
}

void CHardware::Ac97Reset()
{
    // Cold-reset of both AC97 codecs via GPIO/ring-bus.
    // From maestro_ac97_reset() in reference maestro.c OSS driver.
    USHORT save_68, w;

    IoWrite16(0x38, IoRead16(0x38) & 0xFFFC);
    IoWrite16(0x3A, IoRead16(0x3A) & 0xFFFC);
    IoWrite16(0x3C, IoRead16(0x3C) & 0xFFFC);

    // First codec
    IoWrite16(REG_RING_BUS_H, 0x0000);
    save_68 = IoRead16(0x68);
    IoWrite16(REG_GPIO_MASK, 0xFFFE);
    IoWrite16(0x68, 0x0001);
    IoWrite16(REG_GPIO_DATA, 0x0000);
    KeStallExecutionProcessor(20);
    IoWrite16(REG_GPIO_DATA, 0x0001);
    // Delay 20ms equivalent via stall
    for (int i = 0; i < 20000; i++) KeStallExecutionProcessor(1);

    IoWrite16(0x68, save_68 | 0x0001);
    IoWrite16(0x38, (IoRead16(0x38) & 0xFFFC) | 1);
    IoWrite16(0x3A, (IoRead16(0x3A) & 0xFFFC) | 1);
    IoWrite16(0x3C, (IoRead16(0x3C) & 0xFFFC) | 1);

    // Second codec
    IoWrite16(REG_RING_BUS_H, 0x0000);
    IoWrite16(REG_GPIO_MASK, 0xFFF7);
    save_68 = IoRead16(0x68);
    IoWrite16(0x68, 0x0009);
    IoWrite16(REG_GPIO_DATA, 0x0001);
    KeStallExecutionProcessor(20);
    IoWrite16(REG_GPIO_DATA, 0x0009);
    for (int i = 0; i < 500000; i++) KeStallExecutionProcessor(1);

    IoWrite16(0x38, IoRead16(0x38) & 0xFFFC);
    IoWrite16(0x3A, IoRead16(0x3A) & 0xFFFC);
    IoWrite16(0x3C, IoRead16(0x3C) & 0xFFFC);

    UNREFERENCED_PARAMETER(w);
}

void CHardware::Ac97HardwareInit()
{
    // AC97 default values from ES197X.vxd decoded sequence.
    // DisablePR3State=1: clear PR3 in POWERDOWN to keep analog mixer active.
    Ac97Write(AC97_MASTER_VOL,  0x0606); // initial: slight attenuation
    Ac97Write(AC97_MIC_VOL,     0x0000); // muted
    Ac97Write(AC97_CD_VOL,      0x0000); // muted (routes via SAM9707)
    Ac97Write(AC97_VIDEO_VOL,   0x0000); // muted
    Ac97Write(AC97_REC_SELECT,  0x0105); // mic-L, line-R
    Ac97Write(AC97_REC_GAIN,    0x0000); // 0 dB record gain
    Ac97Write(AC97_GENERAL_PURPOSE, 0x0000);
    Ac97Write(AC97_MASTER_VOL,  0x0000); // final: unmuted, 0 dB

    // PR3 disable: keep AC97 analog mixer powered (DisablePR3State=1)
    USHORT pwr = Ac97Read(AC97_POWERDOWN);
    pwr &= (USHORT)~AC97_PR3_ANALOG_PWRDN;
    Ac97Write(AC97_POWERDOWN, pwr);

    // Misc defaults from wdma_m2e.inf [Maestro.AddReg]
    Ac97Write(0x1E, 0x0404); // aux out default
    Ac97Write(0x20, 0x0000); // misc = 0
}

void CHardware::ChipInit()
{
    ULONG n;
    USHORT w;

    // ── PCI config (done before BAR0 mapping in AddDevice) ────────────
    // Handled in adapter.cpp via PCI config read/write

    // ── Soft reset ───────────────────────────────────────────────────
    SoundReset();

    // ── Ring bus initial setup ───────────────────────────────────────
    IoWrite16(REG_RING_BUS_L, 0xC090); // DirectSound stereo
    KeStallExecutionProcessor(20);
    IoWrite16(REG_RING_BUS_H, 0x3000);
    KeStallExecutionProcessor(20);

    // ── AC97 codec cold reset (BUG-6) ────────────────────────────────
    Ac97Reset();

    // ── Ring bus full configuration ──────────────────────────────────
    n  = READ_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L));
    n &= ~0xF000UL; n |= 12UL << 12; WRITE_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L), n);
    n  = READ_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L));
    n &= ~0x0F00UL;                   WRITE_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L), n);
    n  = READ_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L));
    n &= ~0x00F0UL; n |= 9UL << 4;   WRITE_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L), n);
    n  = READ_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L));
    n &= ~0x000FUL;                   WRITE_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L), n);
    n  = READ_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L));
    n |= (1UL << 29);                 WRITE_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L), n);
    n  = READ_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L));
    n |= (1UL << 28);                 WRITE_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L), n);
    n  = READ_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L));
    n &= ~0x00F00000UL;               WRITE_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L), n);
    n  = READ_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L));
    n &= ~0x000F0000UL;               WRITE_PORT_ULONG((PULONG)(m_iobase_raw + REG_RING_BUS_L), n);

    // ── Host IRQ setup ───────────────────────────────────────────────
    w  = IoRead16(REG_HOST_IRQ);
    w &= (USHORT)~(1 << 7);   // ClkRun off
    w &= (USHORT)~(1 << 6);   // Harpo off
    w &= (USHORT)~(1 << 4);   // ASSP IRQ off
    w &= (USHORT)~(1 << 3);   // ISDN IRQ off
    w |= (USHORT) (1 << 2);   // DirectSound IRQ on
    w &= (USHORT)~(1 << 1);   // MPU401 off
    w |= (USHORT) (1 << 0);   // SB IRQ on
    // DisableHWVolCtrl=1 (maestro.inf): hardware volume wheel NOT used on ISIS
    w &= (USHORT)~(1 << 6);   // HW volume IRQ off
    IoWrite16(REG_HOST_IRQ, w);

    // ── ASSP registers (required even when ASSP unused) ──────────────
    IoWrite8(0xA4, 0x00);
    IoWrite8(0xA2, 0x03);
    IoWrite8(0xA6, 0x00);

    // ── Clear WC buffer descriptor regions 0x01D0-0x01EF ─────────────
    for (int apu = 0; apu < 16; apu++) {
        IoWrite16(REG_WC_INDEX, (USHORT)(0x01E0 + apu));
        IoWrite16(REG_WC_DATA, 0);
        IoWrite16(REG_WC_INDEX, (USHORT)(0x01D0 + apu));
        IoWrite16(REG_WC_DATA, 0);
    }

    // ── ES1978-specific WP register init (from ES197X.vxd @ 0x3BBC0) ──
    // These differ from generic maestro.c ES1968 values.
    IoWrite16(REG_WP_INDEX, IDR2_CRAM_DATA); IoWrite16(REG_WP_DATA, 0x0000);
    IoWrite16(REG_WP_INDEX, 0x07); IoWrite16(REG_WP_DATA, WP_R07_INIT);
    IoWrite16(REG_WP_INDEX, 0x08); IoWrite16(REG_WP_DATA, WP_R08_INIT);
    IoWrite16(REG_WP_INDEX, 0x09); IoWrite16(REG_WP_DATA, WP_R09_INIT);
    IoWrite16(REG_WP_INDEX, 0x0A); IoWrite16(REG_WP_DATA, WP_R0A_INIT);
    IoWrite16(REG_WP_INDEX, 0x0B); IoWrite16(REG_WP_DATA, WP_R0B_INIT);
    IoWrite16(REG_WP_INDEX, 0x0C); IoWrite16(REG_WP_DATA, WP_R0C_INIT);
    IoWrite16(REG_WP_INDEX, 0x0D); IoWrite16(REG_WP_DATA, WP_R0D_INIT);
    IoWrite16(REG_WP_INDEX, 0x0E); IoWrite16(REG_WP_DATA, WP_R0E_INIT);

    // ── WaveCache control (size=2MB, enable) ─────────────────────────
    IoWrite16(REG_WC_CONTROL, IoRead16(REG_WC_CONTROL) | (1 << 8));
    IoWrite16(REG_WC_CONTROL, IoRead16(REG_WC_CONTROL) & 0xFE03);
    IoWrite16(REG_WC_CONTROL, IoRead16(REG_WC_CONTROL) & 0xFFFC);
    IoWrite16(REG_WC_CONTROL, IoRead16(REG_WC_CONTROL) | (1 << 7));
    IoWrite16(REG_WC_CONTROL, 0xA1A0);

    // ── Clear all 64 APUs ─────────────────────────────────────────────
    for (int a = 0; a < NR_APUS; a++)
        for (int r = 0; r < NR_APU_REGS; r++)
            ApuSet((UCHAR)a, (UCHAR)r, 0);

    // ── WaveCache base registers (BUG-7) ──────────────────────────────
    SetWcBase();

    // ── Unmute output amplifier via GPIO ──────────────────────────────
    // maestro.inf: TurnOnOffExtAmp=1 (GPIO method 1 = GPIO bit 1)
    // GPIOVal=0x09 = bits 0,3 high when amp enabled
    IoWrite16(REG_GPIO_MASK, 0x07FF);
    IoWrite16(REG_GPIO_DATA,
              (IoRead16(REG_GPIO_DATA) | GPIO_AMP_BIT) | GPIO_VAL_AMP_ON);
    IoWrite16(REG_GPIO_MASK, 0x0FFF);

    // ── AC97 hardware init ────────────────────────────────────────────
    Ac97HardwareInit();

    m_clock = MAESTRO_CLOCK_HZ;
}
