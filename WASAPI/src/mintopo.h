// SPDX-License-Identifier: GPL-2.0-only
/*
 * mintopo.h  —  Topology miniport for Maestro-2E AC97 mixer
 */
#pragma once
#include "hw.h"
#include "adapter.h"

// {B9E5E2B0-1978-125D-0010-14AF0000BEEF}
DEFINE_GUID(CLSID_MiniportTopology_Maestro,
    0xb9e5e2b0, 0x1978, 0x125d,
    0x00, 0x10, 0x14, 0xaf, 0x00, 0x00, 0xbe, 0xef);

// Topology node IDs
enum TOPO_NODE {
    TOPO_WAVEOUT_VOL        = 0,
    TOPO_WAVEOUT_MUTE       = 1,
    TOPO_MASTER_VOL         = 2,
    TOPO_MASTER_MUTE        = 3,
    TOPO_MIC_VOL            = 4,
    TOPO_MIC_MUTE           = 5,
    TOPO_LINEIN_VOL         = 6,
    TOPO_LINEIN_MUTE        = 7,
    TOPO_CD_VOL             = 8,
    TOPO_CD_MUTE            = 9,
    TOPO_RECORD_MUX         = 10,
    TOPO_RECORD_VOL         = 11,
    TOPO_SPDIF_ENABLE       = 12,
    TOPO_NODE_COUNT
};

// Topology pin IDs
enum TOPO_PIN {
    TOPO_BRIDGE_IN_WAVEOUT  = 0,   // wave output → DAC
    TOPO_BRIDGE_IN_MICIN    = 1,   // MIC input
    TOPO_BRIDGE_IN_LINEIN   = 2,   // Line In
    TOPO_BRIDGE_IN_CD       = 3,   // CD audio
    TOPO_BRIDGE_OUT_SPEAKER = 4,   // to speaker/PCM1718E DAC
    TOPO_BRIDGE_OUT_SPDIF   = 5,   // to CS8402 S/PDIF TX
    TOPO_BRIDGE_OUT_RECORD  = 6,   // to capture ADC
    TOPO_PIN_COUNT
};

NTSTATUS NewMiniportTopology_Maestro(
    _Out_ PUNKNOWN*  Unknown,
    _In_  REFCLSID   ClassId,
    _In_  PUNKNOWN   OuterUnknown,
    _In_  POOL_TYPE  PoolType);
