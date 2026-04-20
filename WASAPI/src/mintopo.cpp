// SPDX-License-Identifier: GPL-2.0-only
/*
 * mintopo.cpp  —  Topology miniport for Maestro-2E AC97 mixer
 *                 Guillemot Maxi Studio ISIS WDM driver
 *
 * Implements the AC97 mixer graph:
 *   Wave Out  → [Volume] → [Mute] ─┐
 *   Mic In    → [Volume] → [Mute] ─┤→ [Master Vol] → [Mute] → Speakers
 *   Line In   → [Volume] → [Mute] ─┤                          → S/PDIF
 *   CD In     → [Volume] → [Mute] ─┘
 *   Record MUX (Mic/Line/CD) → [RecVol] → ADC
 *
 * Property handlers translate KSPROPERTY_AUDIO_* to direct AC97 register
 * reads/writes, providing standard Windows mixer control.
 */

#include "mintopo.h"
#include "tables.h"

// ─── AC97 mixer register map ──────────────────────────────────────────────

struct AC97_CONTROL {
    USHORT  reg;        // AC97 register
    BOOLEAN stereo;     // stereo (left + right) control
    ULONG   max;        // max slider value (steps)
    BOOLEAN invert;     // TRUE = 0 is max (volume attenuation style)
};

static const AC97_CONTROL g_AC97Map[TOPO_NODE_COUNT] = {
    { AC97_PCM_OUT_VOL,  TRUE,  31, TRUE  }, // TOPO_WAVEOUT_VOL
    { AC97_PCM_OUT_VOL,  TRUE,   1, FALSE }, // TOPO_WAVEOUT_MUTE
    { AC97_MASTER_VOL,   TRUE,  63, TRUE  }, // TOPO_MASTER_VOL
    { AC97_MASTER_VOL,   TRUE,   1, FALSE }, // TOPO_MASTER_MUTE
    { AC97_MIC_VOL,      FALSE, 31, TRUE  }, // TOPO_MIC_VOL
    { AC97_MIC_VOL,      FALSE,  1, FALSE }, // TOPO_MIC_MUTE
    { AC97_LINE_VOL,     TRUE,  31, TRUE  }, // TOPO_LINEIN_VOL
    { AC97_LINE_VOL,     TRUE,   1, FALSE }, // TOPO_LINEIN_MUTE
    { AC97_CD_VOL,       TRUE,  31, TRUE  }, // TOPO_CD_VOL
    { AC97_CD_VOL,       TRUE,   1, FALSE }, // TOPO_CD_MUTE
    { AC97_REC_SELECT,   TRUE,   7, FALSE }, // TOPO_RECORD_MUX
    { AC97_REC_GAIN,     TRUE,  15, FALSE }, // TOPO_RECORD_VOL
    { 0,                 FALSE,  1, FALSE }, // TOPO_SPDIF_ENABLE (GPIO)
};

// ─── CMaestroTopology ─────────────────────────────────────────────────────

class CMaestroTopology
    : public IMiniportTopology
    , public CUnknown
{
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMaestroTopology);
    ~CMaestroTopology() {}

    CHardware* m_pHW;

    // ── Property handler helpers ─────────────────────────────────────
    NTSTATUS HandleVolume(
        _In_ PPCPROPERTY_REQUEST Request,
        _In_ ULONG NodeId,
        _In_ BOOLEAN bGet);

    NTSTATUS HandleMute(
        _In_ PPCPROPERTY_REQUEST Request,
        _In_ ULONG NodeId,
        _In_ BOOLEAN bGet);

    NTSTATUS HandleMux(
        _In_ PPCPROPERTY_REQUEST Request,
        _In_ BOOLEAN bGet);

    // Static dispatchers
    static NTSTATUS PropertyHandlerVolume(
        _In_ PPCPROPERTY_REQUEST Request);
    static NTSTATUS PropertyHandlerMute(
        _In_ PPCPROPERTY_REQUEST Request);
    static NTSTATUS PropertyHandlerMux(
        _In_ PPCPROPERTY_REQUEST Request);

public:
    STDMETHODIMP_(NTSTATUS) Init(
        _In_ PUNKNOWN      UnknownAdapter,
        _In_ PRESOURCELIST ResourceList,
        _In_ PPORTTOPOLOGY Port);

    STDMETHODIMP_(NTSTATUS) GetDescription(
        _Out_ PPCFILTER_DESCRIPTOR* Description);

    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(
        _In_        ULONG           PinId,
        _In_        PKSDATARANGE    ClientDataRange,
        _In_        PKSDATARANGE    MyDataRange,
        _In_        ULONG           OutputBufferLength,
        _Out_opt_   PVOID           ResultantFormat,
        _Out_       PULONG          ResultantFormatLength);
};

STDMETHODIMP_(NTSTATUS) CMaestroTopology::Init(
    _In_ PUNKNOWN      UnknownAdapter,
    _In_ PRESOURCELIST /*ResourceList*/,
    _In_ PPORTTOPOLOGY /*Port*/)
{
    PADAPTERCOMMON pAdapter = nullptr;
    NTSTATUS status = UnknownAdapter->QueryInterface(
        IID_IAdapterCommon, (PVOID*)&pAdapter);
    if (!NT_SUCCESS(status)) return status;
    m_pHW = pAdapter->GetHardware();
    pAdapter->Release();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMaestroTopology::GetDescription(
    _Out_ PPCFILTER_DESCRIPTOR* Description)
{
    *Description = &g_TopoFilterDescriptor; // defined in tables.cpp
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMaestroTopology::DataRangeIntersection(
    _In_        ULONG        PinId,
    _In_        PKSDATARANGE ClientDataRange,
    _In_        PKSDATARANGE MyDataRange,
    _In_        ULONG        OutputBufferLength,
    _Out_opt_   PVOID        ResultantFormat,
    _Out_       PULONG       ResultantFormatLength)
{
    return PcDefaultDataRangeIntersection(
        PinId, ClientDataRange, MyDataRange,
        OutputBufferLength, ResultantFormat, ResultantFormatLength);
}

// ─── Volume property handler ──────────────────────────────────────────────

NTSTATUS CMaestroTopology::HandleVolume(
    _In_ PPCPROPERTY_REQUEST Request,
    _In_ ULONG NodeId,
    _In_ BOOLEAN bGet)
{
    if (NodeId >= TOPO_NODE_COUNT) return STATUS_INVALID_PARAMETER;

    const AC97_CONTROL& ctrl = g_AC97Map[NodeId];
    PKSPROPERTY_AUDIO_VOLUMELEVEL pVol =
        (PKSPROPERTY_AUDIO_VOLUMELEVEL)Request->Value;

    USHORT raw = m_pHW->Ac97Read(ctrl.reg);

    if (bGet) {
        // Convert AC97 attenuation (0=max, N=silent) to dB * 65536
        ULONG atten = ctrl.stereo ?
            ((raw >> 8) & ctrl.max) : (raw & ctrl.max);
        if (ctrl.invert)
            pVol->Level = (LONG)((ctrl.max - atten) * 65536L /
                                  ctrl.max * (-6));  // ~1.5 dB per step
        else
            pVol->Level = 0;
    } else {
        // Convert dB level to AC97 attenuation
        LONG dbx65536 = pVol->Level;
        // Clamp to [-94 dB, 0 dB]
        if (dbx65536 > 0) dbx65536 = 0;
        if (dbx65536 < (-94 * 65536L)) dbx65536 = -94 * 65536L;

        ULONG atten = (ULONG)((-dbx65536 * (LONG)ctrl.max) / (94 * 65536L));
        if (ctrl.stereo) {
            raw = (USHORT)((atten << 8) | atten);
        } else {
            raw = (USHORT)((raw & 0xFF00) | atten);
        }
        m_pHW->Ac97Write(ctrl.reg, raw);
    }
    return STATUS_SUCCESS;
}

// ─── Mute property handler ────────────────────────────────────────────────

NTSTATUS CMaestroTopology::HandleMute(
    _In_ PPCPROPERTY_REQUEST Request,
    _In_ ULONG NodeId,
    _In_ BOOLEAN bGet)
{
    if (NodeId >= TOPO_NODE_COUNT) return STATUS_INVALID_PARAMETER;

    const AC97_CONTROL& ctrl = g_AC97Map[NodeId];
    PBOOL pMute = (PBOOL)Request->Value;

    USHORT raw = m_pHW->Ac97Read(ctrl.reg);

    if (bGet) {
        *pMute = (raw & AC97_MUTE) ? TRUE : FALSE;
    } else {
        if (*pMute)
            raw |= AC97_MUTE;
        else
            raw &= (USHORT)~AC97_MUTE;
        m_pHW->Ac97Write(ctrl.reg, raw);
    }
    return STATUS_SUCCESS;
}

// ─── MUX property handler (record source selection) ───────────────────────

NTSTATUS CMaestroTopology::HandleMux(
    _In_ PPCPROPERTY_REQUEST Request,
    _In_ BOOLEAN bGet)
{
    PKSMUXINPUT pMux = (PKSMUXINPUT)Request->Value;

    if (bGet) {
        USHORT raw = m_pHW->Ac97Read(AC97_REC_SELECT);
        pMux->PinId = (raw & 0x7);  // left channel select
    } else {
        // Map: 0=Mic, 1=CD, 2=Video, 3=AUX, 4=Line, 5=Mix, 6=MonoMix, 7=Phone
        USHORT sel = (USHORT)(pMux->PinId & 0x7);
        USHORT val = (USHORT)((sel << 8) | sel);  // same for L and R
        m_pHW->Ac97Write(AC97_REC_SELECT, val);
    }
    return STATUS_SUCCESS;
}

// ─── Static property dispatchers ──────────────────────────────────────────

NTSTATUS CMaestroTopology::PropertyHandlerVolume(
    _In_ PPCPROPERTY_REQUEST Request)
{
    CMaestroTopology* self =
        (CMaestroTopology*)Request->MajorTarget;
    BOOLEAN bGet =
        (Request->Verb & KSPROPERTY_TYPE_GET) ? TRUE : FALSE;
    return self->HandleVolume(Request, Request->Node, bGet);
}

NTSTATUS CMaestroTopology::PropertyHandlerMute(
    _In_ PPCPROPERTY_REQUEST Request)
{
    CMaestroTopology* self =
        (CMaestroTopology*)Request->MajorTarget;
    BOOLEAN bGet =
        (Request->Verb & KSPROPERTY_TYPE_GET) ? TRUE : FALSE;
    return self->HandleMute(Request, Request->Node, bGet);
}

NTSTATUS CMaestroTopology::PropertyHandlerMux(
    _In_ PPCPROPERTY_REQUEST Request)
{
    CMaestroTopology* self =
        (CMaestroTopology*)Request->MajorTarget;
    BOOLEAN bGet =
        (Request->Verb & KSPROPERTY_TYPE_GET) ? TRUE : FALSE;
    return self->HandleMux(Request, bGet);
}

// ─── Factory ─────────────────────────────────────────────────────────────

NTSTATUS NewMiniportTopology_Maestro(
    _Out_ PUNKNOWN*  Unknown,
    _In_  REFCLSID   /*ClassId*/,
    _In_  PUNKNOWN   OuterUnknown,
    _In_  POOL_TYPE  PoolType)
{
    CMaestroTopology* p =
        new(PoolType, 'TrtsM') CMaestroTopology(OuterUnknown);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    *Unknown = (PUNKNOWN)(IMiniportTopology*)p;
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

DECLARE_CLASSFACTORY_CUSTOM(CLSID_MiniportTopology_Maestro,
                             NewMiniportTopology_Maestro)
