// SPDX-License-Identifier: GPL-2.0-only
/*
 * minwave.cpp  —  WaveRT miniport implementation
 *                 Guillemot Maxi Studio ISIS WDM driver
 *
 * Implements IMiniportWaveRT (required for WASAPI shared + exclusive mode)
 * and IMiniportWaveRTStreamNotification (required for WASAPI exclusive).
 * The hardware DMA buffer is allocated with 28-bit physical constraint and
 * mapped into user space via the WaveRT port's MMIO mechanism.
 */

#include "minwave.h"
#include "tables.h"

// ─── CMaestroWaveRTStream ─────────────────────────────────────────────────

class CMaestroWaveRTStream
    : public IMiniportWaveRTStreamNotification
    , public CUnknown
{
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMaestroWaveRTStream);
    ~CMaestroWaveRTStream();

    CHardware*      m_pHW;
    STREAM_DESC     m_SD;
    BOOLEAN         m_bCapture;
    ULONG           m_nChannels;
    ULONG           m_nSampleRate;
    ULONG           m_nBitsPerSample;
    PMDL            m_pMDL;
    ULONG           m_BufferSize;
    PKEVENT         m_pNotificationEvent;
    KTIMER          m_NotificationTimer;
    KDPC            m_NotificationDpc;
    BOOL            m_bTimerRunning;

    static void NTAPI NotificationDpc(
        _In_ PKDPC Dpc,
        _In_opt_ PVOID DeferredContext,
        _In_opt_ PVOID /*Arg1*/,
        _In_opt_ PVOID /*Arg2*/);

public:
    NTSTATUS Init(
        _In_ CHardware*           pHW,
        _In_ ULONG                Pin,
        _In_ BOOLEAN              bCapture,
        _In_ PKSDATAFORMAT        DataFormat,
        _In_ PPORTWAVERTSTREAM    PortStream);

    // IMiniportWaveRTStream
    STDMETHODIMP_(NTSTATUS) SetFormat(
        _In_ PKSDATAFORMAT DataFormat);

    STDMETHODIMP_(NTSTATUS) AllocateAudioBuffer(
        _In_  ULONG               RequestedSize,
        _Out_ PMDL*               AudioBufferMdl,
        _Out_ ULONG*              ActualSize,
        _Out_ ULONG*              OffsetFromFirstPage,
        _Out_ MEMORY_CACHING_TYPE* CacheType);

    STDMETHODIMP_(void) FreeAudioBuffer(
        _In_ PMDL  AudioBufferMdl,
        _In_ ULONG BufferSize);

    STDMETHODIMP_(void) GetHardwareLatency(
        _Out_ KSTIME* Hardware);

    STDMETHODIMP_(NTSTATUS) GetPositionRegister(
        _Out_ PKSAUDIO_POSITION Register);

    STDMETHODIMP_(NTSTATUS) GetClockRegister(
        _Out_ PKSAUDIO_CLOCKREGISTER Register);

    STDMETHODIMP_(NTSTATUS) SetState(
        _In_ KSSTATE State);

    // IMiniportWaveRTStreamNotification
    STDMETHODIMP_(NTSTATUS) RegisterNotificationEvent(
        _In_ PKEVENT NotificationEvent);

    STDMETHODIMP_(NTSTATUS) UnregisterNotificationEvent(
        _In_ PKEVENT NotificationEvent);
};

// ─── CMaestroWaveRTStream implementation ──────────────────────────────────

CMaestroWaveRTStream::~CMaestroWaveRTStream()
{
    if (m_bTimerRunning) {
        KeCancelTimer(&m_NotificationTimer);
        m_bTimerRunning = FALSE;
    }
    m_pHW->StopStream(&m_SD);
    m_pHW->UnregisterStream(&m_SD);
    m_pHW->FreeApus(m_SD.apu[0], m_bCapture ? 4 : 2);
}

NTSTATUS CMaestroWaveRTStream::Init(
    _In_ CHardware*        pHW,
    _In_ ULONG             Pin,
    _In_ BOOLEAN           bCapture,
    _In_ PKSDATAFORMAT     DataFormat,
    _In_ PPORTWAVERTSTREAM PortStream)
{
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(PortStream);

    m_pHW      = pHW;
    m_bCapture = bCapture;
    m_bTimerRunning = FALSE;
    RtlZeroMemory(&m_SD, sizeof(m_SD));

    // Extract format parameters
    PWAVEFORMATEXTENSIBLE pWfx =
        (PWAVEFORMATEXTENSIBLE)(DataFormat + 1);

    m_nChannels       = pWfx->Format.nChannels;
    m_nSampleRate     = pWfx->Format.nSamplesPerSec;
    m_nBitsPerSample  = pWfx->Format.wBitsPerSample;

    // Default buffer/period
    m_SD.dma_size  = m_nSampleRate * m_nChannels * (m_nBitsPerSample / 8) / 10; // 100ms
    m_SD.dma_size  = (m_SD.dma_size + DMA_ALIGN - 1) & ~(DMA_ALIGN - 1);
    m_SD.frag_size = m_SD.dma_size / 4;

    // Initialize DPC for notifications
    KeInitializeDpc(&m_NotificationDpc, NotificationDpc, this);
    KeInitializeTimer(&m_NotificationTimer);

    pHW->RegisterStream(&m_SD);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMaestroWaveRTStream::SetFormat(
    _In_ PKSDATAFORMAT DataFormat)
{
    PWAVEFORMATEXTENSIBLE pWfx = (PWAVEFORMATEXTENSIBLE)(DataFormat + 1);
    m_nChannels      = pWfx->Format.nChannels;
    m_nSampleRate    = pWfx->Format.nSamplesPerSec;
    m_nBitsPerSample = pWfx->Format.wBitsPerSample;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMaestroWaveRTStream::AllocateAudioBuffer(
    _In_  ULONG               RequestedSize,
    _Out_ PMDL*               AudioBufferMdl,
    _Out_ ULONG*              ActualSize,
    _Out_ ULONG*              OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE* CacheType)
{
    // Allocate DMA memory within 28-bit addressing constraint
    PHYSICAL_ADDRESS lo, hi, align;
    lo.QuadPart    = 0;
    hi.QuadPart    = (1ULL << 28) - 1;
    align.QuadPart = DMA_ALIGN;

    ULONG size = (RequestedSize + DMA_ALIGN - 1) & ~(DMA_ALIGN - 1);

    PVOID pBuffer = MmAllocateContiguousMemorySpecifyCache(
        size, lo, hi, align, MmNonCached);
    if (!pBuffer) return STATUS_INSUFFICIENT_RESOURCES;

    PHYSICAL_ADDRESS phys = MmGetPhysicalAddress(pBuffer);

    // Create MDL for the buffer
    PMDL pMdl = IoAllocateMdl(pBuffer, size, FALSE, FALSE, nullptr);
    if (!pMdl) {
        MmFreeContiguousMemory(pBuffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(pMdl);

    m_SD.dma_phys = phys;
    m_SD.dma_virt = pBuffer;
    m_SD.dma_size = size;
    m_SD.frag_size = size / 4;
    m_pMDL = pMdl;
    m_BufferSize = size;

    // Allocate mixbuf for capture
    if (m_bCapture) {
        PVOID mixbuf = MmAllocateContiguousMemorySpecifyCache(
            MIXBUF_SIZE, lo, hi, align, MmNonCached);
        if (mixbuf) {
            m_SD.mixbuf_phys  = MmGetPhysicalAddress(mixbuf);
            m_SD.mixbuf_virt  = mixbuf;
            m_SD.mixbuf_size  = MIXBUF_SIZE;
            RtlZeroMemory(mixbuf, MIXBUF_SIZE);
        }
    }

    RtlZeroMemory(pBuffer, size);

    // Setup hardware APUs for this stream
    NTSTATUS status;
    if (m_bCapture) {
        status = m_pHW->SetupCapture(&m_SD, m_nSampleRate,
                                      m_nChannels, m_nBitsPerSample);
    } else {
        status = m_pHW->SetupPlayback(&m_SD, m_nSampleRate,
                                       m_nChannels, m_nBitsPerSample);
    }

    if (!NT_SUCCESS(status)) {
        IoFreeMdl(pMdl);
        MmFreeContiguousMemory(pBuffer);
        if (m_SD.mixbuf_virt)
            MmFreeContiguousMemory(m_SD.mixbuf_virt);
        return status;
    }

    *AudioBufferMdl      = pMdl;
    *ActualSize          = size;
    *OffsetFromFirstPage = 0;
    *CacheType           = MmNonCached;

    return STATUS_SUCCESS;
}

STDMETHODIMP_(void) CMaestroWaveRTStream::FreeAudioBuffer(
    _In_ PMDL  AudioBufferMdl,
    _In_ ULONG /*BufferSize*/)
{
    m_pHW->StopStream(&m_SD);

    if (AudioBufferMdl) {
        PVOID buf = MmGetMdlVirtualAddress(AudioBufferMdl);
        IoFreeMdl(AudioBufferMdl);
        if (buf) MmFreeContiguousMemory(buf);
    }
    if (m_SD.mixbuf_virt) {
        MmFreeContiguousMemory(m_SD.mixbuf_virt);
        m_SD.mixbuf_virt = nullptr;
    }
    m_pMDL = nullptr;
}

STDMETHODIMP_(void) CMaestroWaveRTStream::GetHardwareLatency(
    _Out_ KSTIME* Hardware)
{
    // Maestro-2E hardware latency ≈ one DMA period
    // At 48 kHz, 256 frames → ~5.3 ms
    Hardware->Time  = 53000;   // 100-ns units → 5.3 ms
    Hardware->Numerator   = 1;
    Hardware->Denominator = 1;
}

STDMETHODIMP_(NTSTATUS) CMaestroWaveRTStream::GetPositionRegister(
    _Out_ PKSAUDIO_POSITION Register)
{
    // WaveRT position: hardware returns byte position in the cyclic buffer.
    // The APU position register (reg 5) gives word offset from buffer start.
    // We expose current byte position read directly from hardware.
    Register->PlayOffset    = m_pHW->GetStreamPosition(&m_SD);
    Register->WriteOffset   = Register->PlayOffset;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMaestroWaveRTStream::GetClockRegister(
    _Out_ PKSAUDIO_CLOCKREGISTER /*Register*/)
{
    // ES1978 does not expose a memory-mapped clock counter register.
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS) CMaestroWaveRTStream::SetState(
    _In_ KSSTATE State)
{
    switch (State) {
    case KSSTATE_STOP:
        if (m_bTimerRunning) {
            KeCancelTimer(&m_NotificationTimer);
            m_bTimerRunning = FALSE;
        }
        m_pHW->StopStream(&m_SD);
        break;

    case KSSTATE_ACQUIRE:
    case KSSTATE_PAUSE:
        m_pHW->StopStream(&m_SD);
        break;

    case KSSTATE_RUN:
        m_pHW->StartStream(&m_SD);

        // Start period-elapsed timer for WASAPI notifications
        if (m_pNotificationEvent && m_SD.frag_size) {
            // Period in 100-ns units
            LONGLONG period_100ns =
                (LONGLONG)m_SD.frag_size * 10000000LL /
                (m_nSampleRate * m_nChannels * (m_nBitsPerSample / 8));

            LARGE_INTEGER due;
            due.QuadPart = -period_100ns;
            KeSetTimerEx(&m_NotificationTimer, due,
                         (LONG)(period_100ns / 10000), // ms
                         &m_NotificationDpc);
            m_bTimerRunning = TRUE;
        }
        break;
    }
    return STATUS_SUCCESS;
}

// ─── Notification ─────────────────────────────────────────────────────────

STDMETHODIMP_(NTSTATUS) CMaestroWaveRTStream::RegisterNotificationEvent(
    _In_ PKEVENT NotificationEvent)
{
    m_pNotificationEvent = NotificationEvent;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMaestroWaveRTStream::UnregisterNotificationEvent(
    _In_ PKEVENT /*NotificationEvent*/)
{
    m_pNotificationEvent = nullptr;
    return STATUS_SUCCESS;
}

void NTAPI CMaestroWaveRTStream::NotificationDpc(
    _In_ PKDPC            /*Dpc*/,
    _In_opt_ PVOID        DeferredContext,
    _In_opt_ PVOID        /*Arg1*/,
    _In_opt_ PVOID        /*Arg2*/)
{
    CMaestroWaveRTStream* stream =
        (CMaestroWaveRTStream*)DeferredContext;

    if (stream && stream->m_pNotificationEvent)
        KeSetEvent(stream->m_pNotificationEvent, 0, FALSE);
}

// ─── CMaestroWaveRT (miniport) ────────────────────────────────────────────

class CMaestroWaveRT
    : public IMiniportWaveRT
    , public CUnknown
{
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMaestroWaveRT);
    ~CMaestroWaveRT() { if (m_pHW) m_pHW = nullptr; }

    CHardware*          m_pHW;
    PPORTWAVERT         m_pPort;

public:
    // IMiniport
    STDMETHODIMP_(NTSTATUS) GetDescription(
        _Out_ PPCFILTER_DESCRIPTOR* Description);

    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(
        _In_        ULONG           PinId,
        _In_        PKSDATARANGE    ClientDataRange,
        _In_        PKSDATARANGE    MyDataRange,
        _In_        ULONG           OutputBufferLength,
        _Out_opt_   PVOID           ResultantFormat,
        _Out_       PULONG          ResultantFormatLength);

    // IMiniportWaveRT
    STDMETHODIMP_(NTSTATUS) Init(
        _In_ PUNKNOWN       UnknownAdapter,
        _In_ PRESOURCELIST  ResourceList,
        _In_ PPORTWAVERT    Port);

    STDMETHODIMP_(NTSTATUS) NewStream(
        _Out_ PMINIPORTWAVERTSTREAM* Stream,
        _In_  PPORTWAVERTSTREAM      PortStream,
        _In_  ULONG                  Pin,
        _In_  BOOLEAN                Capture,
        _In_  PKSDATAFORMAT          DataFormat);

    STDMETHODIMP_(void) GetDeviceDescription(
        _Out_ PDEVICE_DESCRIPTION DeviceDescription);
};

STDMETHODIMP_(NTSTATUS) CMaestroWaveRT::Init(
    _In_ PUNKNOWN       UnknownAdapter,
    _In_ PRESOURCELIST  /*ResourceList*/,
    _In_ PPORTWAVERT    Port)
{
    PADAPTERCOMMON pAdapter = nullptr;
    NTSTATUS status = UnknownAdapter->QueryInterface(
        IID_IAdapterCommon, (PVOID*)&pAdapter);
    if (!NT_SUCCESS(status)) return status;

    m_pHW   = pAdapter->GetHardware();
    m_pPort = Port;
    Port->AddRef();
    pAdapter->Release();

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMaestroWaveRT::NewStream(
    _Out_ PMINIPORTWAVERTSTREAM* Stream,
    _In_  PPORTWAVERTSTREAM      PortStream,
    _In_  ULONG                  Pin,
    _In_  BOOLEAN                Capture,
    _In_  PKSDATAFORMAT          DataFormat)
{
    CMaestroWaveRTStream* pStream =
        new(NonPagedPoolNx, 'WrtsM') CMaestroWaveRTStream(nullptr);
    if (!pStream) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = pStream->Init(
        m_pHW, Pin, Capture, DataFormat, PortStream);
    if (!NT_SUCCESS(status)) {
        pStream->Release();
        return status;
    }

    *Stream = (PMINIPORTWAVERTSTREAM)pStream;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMaestroWaveRT::GetDescription(
    _Out_ PPCFILTER_DESCRIPTOR* Description)
{
    *Description = &g_WaveFilterDescriptor;  // defined in tables.cpp
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMaestroWaveRT::DataRangeIntersection(
    _In_        ULONG           PinId,
    _In_        PKSDATARANGE    ClientDataRange,
    _In_        PKSDATARANGE    MyDataRange,
    _In_        ULONG           OutputBufferLength,
    _Out_opt_   PVOID           ResultantFormat,
    _Out_       PULONG          ResultantFormatLength)
{
    return PcDefaultDataRangeIntersection(
        PinId, ClientDataRange, MyDataRange,
        OutputBufferLength, ResultantFormat, ResultantFormatLength);
}

STDMETHODIMP_(void) CMaestroWaveRT::GetDeviceDescription(
    _Out_ PDEVICE_DESCRIPTION DeviceDescription)
{
    RtlZeroMemory(DeviceDescription, sizeof(*DeviceDescription));
    DeviceDescription->Master    = FALSE;
    DeviceDescription->ScatterGather = FALSE;
    DeviceDescription->Dma32BitAddresses = TRUE;
    DeviceDescription->MaximumLength = DMA_POOL_SIZE_BYTES;
    DeviceDescription->DmaWidth  = Width32Bits;
    DeviceDescription->DmaSpeed  = Compatible;
    DeviceDescription->DmaPort   = 0;
}

NTSTATUS NewMiniportWaveRT_Maestro(
    _Out_ PUNKNOWN*  Unknown,
    _In_  REFCLSID   /*ClassId*/,
    _In_  PUNKNOWN   OuterUnknown,
    _In_  POOL_TYPE  PoolType)
{
    CMaestroWaveRT* p =
        new(PoolType, 'WrtsM') CMaestroWaveRT(OuterUnknown);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    *Unknown = (PUNKNOWN)(IMiniportWaveRT*)p;
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

// COM factory registration
DECLARE_CLASSFACTORY_CUSTOM(CLSID_MiniportWaveRT_Maestro,
                             NewMiniportWaveRT_Maestro)
