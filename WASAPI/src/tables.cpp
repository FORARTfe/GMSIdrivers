// SPDX-License-Identifier: GPL-2.0-only
/*
 * tables.cpp  —  KS filter descriptor tables
 *                Guillemot Maxi Studio ISIS WDM driver
 *
 * Defines the complete KS filter graph for:
 *  1. WaveRT filter   (PCM render + capture pins)
 *  2. Topology filter (AC97 mixer nodes and connections)
 *
 * The topology reflects the actual ISIS hardware:
 *  - PCM3001E AC97 codec (mainboard)
 *  - PCM1718E 18-bit DAC output
 *  - CS8402 S/PDIF transmitter (external box)
 *  - CS8414 S/PDIF receiver (external box)
 *  - 8 inputs + 4 outputs capability
 */

#include "tables.h"
#include "minwave.h"
#include "mintopo.h"

// ─── Data Ranges ─────────────────────────────────────────────────────────

static KSDATARANGE_AUDIO g_WaveRanges[] =
{
    // 48 kHz stereo 16-bit
    {
        { sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
          STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
          STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
          STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
        2,      // max channels
        16,     // min bits
        16,     // max bits
        8000,   // min rate
        48000   // max rate
    },
    // Up to stereo 24-bit for PCM1728E ext DAC
    {
        { sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
          STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
          STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
          STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
        2, 16, 24, 8000, 48000
    },
};

static KSDATARANGE g_BridgeDataRange =
{
    sizeof(KSDATARANGE), 0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};

static PKSDATARANGE g_WaveRangePtrs[] =
{
    (PKSDATARANGE)&g_WaveRanges[0],
    (PKSDATARANGE)&g_WaveRanges[1],
};

static PKSDATARANGE g_BridgeRangePtr[] = { &g_BridgeDataRange };

// ─── WaveRT Pin Descriptors ───────────────────────────────────────────────

static PCPIN_DESCRIPTOR g_WavePins[] =
{
    // Pin 0: Render (playback) — host → DAC
    {
        1,          // MaxInstances (one playback stream at a time)
        1,          // MinInstances
        0,          // reserved
        {           // AutomationTable
            0, nullptr, 0, nullptr, 0, nullptr
        },
        {           // KsPinDescriptor
            0, nullptr,     // interfaces
            0, nullptr,     // mediums
            SIZEOF_ARRAY(g_WaveRangePtrs), g_WaveRangePtrs,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSNODETYPE_AUDIO_ENGINE,
            nullptr,
            0
        }
    },
    // Pin 1: Capture — ADC → host
    {
        1, 1, 0,
        { 0, nullptr, 0, nullptr, 0, nullptr },
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(g_WaveRangePtrs), g_WaveRangePtrs,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SOURCE,
            &KSNODETYPE_MICROPHONE,
            nullptr, 0
        }
    },
    // Pin 2: Bridge to topology (render output)
    {
        0, 0, 0,
        { 0, nullptr, 0, nullptr, 0, nullptr },
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(g_BridgeRangePtr), g_BridgeRangePtr,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_LINE_CONNECTOR,
            nullptr, 0
        }
    },
    // Pin 3: Bridge from topology (capture input)
    {
        0, 0, 0,
        { 0, nullptr, 0, nullptr, 0, nullptr },
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(g_BridgeRangePtr), g_BridgeRangePtr,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_LINE_CONNECTOR,
            nullptr, 0
        }
    },
};

// ─── WaveRT Connection Table ──────────────────────────────────────────────

static PCCONNECTION_DESCRIPTOR g_WaveConnections[] =
{
    { PCFILTER_NODE, 0, PCFILTER_NODE, 2 },  // render host→bridge
    { PCFILTER_NODE, 3, PCFILTER_NODE, 1 },  // bridge→capture host
};

// ─── WaveRT Filter Descriptor ─────────────────────────────────────────────

PCFILTER_DESCRIPTOR g_WaveFilterDescriptor =
{
    0,                              // version
    nullptr,                        // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_WavePins),
    g_WavePins,
    0, nullptr,                     // nodes
    sizeof(PCCONNECTION_DESCRIPTOR),
    SIZEOF_ARRAY(g_WaveConnections),
    g_WaveConnections,
    0                               // CategoryCount
};

// ─── Topology Volume Property Table ──────────────────────────────────────

// Volume property item for audio nodes
static PCPROPERTY_ITEM g_VolPropItems[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_VOLUMELEVEL,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        CMaestroTopology::PropertyHandlerVolume
    }
};

static PCPROPERTY_ITEM g_MutePropItems[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_MUTE,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        CMaestroTopology::PropertyHandlerMute
    }
};

static PCPROPERTY_ITEM g_MuxPropItems[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_MUX_SOURCE,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        CMaestroTopology::PropertyHandlerMux
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(g_VolAutomation,  g_VolPropItems);
DEFINE_PCAUTOMATION_TABLE_PROP(g_MuteAutomation, g_MutePropItems);
DEFINE_PCAUTOMATION_TABLE_PROP(g_MuxAutomation,  g_MuxPropItems);

// ─── Topology Node Descriptors ────────────────────────────────────────────

static PCNODE_DESCRIPTOR g_TopoNodes[] =
{
    // TOPO_WAVEOUT_VOL (0)
    { 0, &g_VolAutomation,  &KSNODETYPE_VOLUME,     nullptr },
    // TOPO_WAVEOUT_MUTE (1)
    { 0, &g_MuteAutomation, &KSNODETYPE_MUTE,       nullptr },
    // TOPO_MASTER_VOL (2)
    { 0, &g_VolAutomation,  &KSNODETYPE_VOLUME,     nullptr },
    // TOPO_MASTER_MUTE (3)
    { 0, &g_MuteAutomation, &KSNODETYPE_MUTE,       nullptr },
    // TOPO_MIC_VOL (4)
    { 0, &g_VolAutomation,  &KSNODETYPE_VOLUME,     nullptr },
    // TOPO_MIC_MUTE (5)
    { 0, &g_MuteAutomation, &KSNODETYPE_MUTE,       nullptr },
    // TOPO_LINEIN_VOL (6)
    { 0, &g_VolAutomation,  &KSNODETYPE_VOLUME,     nullptr },
    // TOPO_LINEIN_MUTE (7)
    { 0, &g_MuteAutomation, &KSNODETYPE_MUTE,       nullptr },
    // TOPO_CD_VOL (8)
    { 0, &g_VolAutomation,  &KSNODETYPE_VOLUME,     nullptr },
    // TOPO_CD_MUTE (9)
    { 0, &g_MuteAutomation, &KSNODETYPE_MUTE,       nullptr },
    // TOPO_RECORD_MUX (10)
    { 0, &g_MuxAutomation,  &KSNODETYPE_MUX_INPUT,  nullptr },
    // TOPO_RECORD_VOL (11)
    { 0, &g_VolAutomation,  &KSNODETYPE_VOLUME,     nullptr },
    // TOPO_SPDIF_ENABLE (12)
    { 0, &g_MuteAutomation, &KSNODETYPE_MUTE,       nullptr },
};

// ─── Topology Pin Descriptors ─────────────────────────────────────────────

static PCPIN_DESCRIPTOR g_TopoPins[] =
{
    // Pin 0: Wave Out bridge (from wave filter)
    { 0, 0, 0, {0,nullptr,0,nullptr,0,nullptr},
      { 0,nullptr,0,nullptr, 1,g_BridgeRangePtr,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
        &KSNODETYPE_LINE_CONNECTOR, nullptr, 0 } },
    // Pin 1: Mic In bridge
    { 0, 0, 0, {0,nullptr,0,nullptr,0,nullptr},
      { 0,nullptr,0,nullptr, 1,g_BridgeRangePtr,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
        &KSNODETYPE_MICROPHONE, nullptr, 0 } },
    // Pin 2: Line In bridge
    { 0, 0, 0, {0,nullptr,0,nullptr,0,nullptr},
      { 0,nullptr,0,nullptr, 1,g_BridgeRangePtr,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
        &KSNODETYPE_LINE_CONNECTOR, nullptr, 0 } },
    // Pin 3: CD In bridge
    { 0, 0, 0, {0,nullptr,0,nullptr,0,nullptr},
      { 0,nullptr,0,nullptr, 1,g_BridgeRangePtr,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
        &KSNODETYPE_CD_PLAYER, nullptr, 0 } },
    // Pin 4: Speaker out (to PCM1718E DAC)
    { 0, 0, 0, {0,nullptr,0,nullptr,0,nullptr},
      { 0,nullptr,0,nullptr, 1,g_BridgeRangePtr,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
        &KSNODETYPE_SPEAKER, nullptr, 0 } },
    // Pin 5: S/PDIF out (to CS8402 — external box)
    { 0, 0, 0, {0,nullptr,0,nullptr,0,nullptr},
      { 0,nullptr,0,nullptr, 1,g_BridgeRangePtr,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
        &KSNODETYPE_SPDIF_INTERFACE, nullptr, 0 } },
    // Pin 6: Record bridge (to wave capture)
    { 0, 0, 0, {0,nullptr,0,nullptr,0,nullptr},
      { 0,nullptr,0,nullptr, 1,g_BridgeRangePtr,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
        &KSNODETYPE_LINE_CONNECTOR, nullptr, 0 } },
};

// ─── Topology Connection Table ────────────────────────────────────────────
// Encodes the full AC97 mixer graph.

static PCCONNECTION_DESCRIPTOR g_TopoConnections[] =
{
    // Wave Out → Volume → Mute → Master
    { PCFILTER_NODE, 0, TOPO_WAVEOUT_VOL,  1  },
    { TOPO_WAVEOUT_VOL,  1,  TOPO_WAVEOUT_MUTE, 1  },
    { TOPO_WAVEOUT_MUTE, 1,  TOPO_MASTER_VOL,   1  },

    // Mic In → MicVol → MicMute → Master
    { PCFILTER_NODE, 1, TOPO_MIC_VOL,   1  },
    { TOPO_MIC_VOL,   1,  TOPO_MIC_MUTE,  1  },
    { TOPO_MIC_MUTE,  1,  TOPO_MASTER_VOL, 2 },

    // Line In → LineVol → LineMute → Master
    { PCFILTER_NODE, 2, TOPO_LINEIN_VOL,  1 },
    { TOPO_LINEIN_VOL,  1, TOPO_LINEIN_MUTE, 1 },
    { TOPO_LINEIN_MUTE, 1, TOPO_MASTER_VOL,  3 },

    // CD In → CDVol → CDMute → Master
    { PCFILTER_NODE, 3, TOPO_CD_VOL,   1 },
    { TOPO_CD_VOL,   1, TOPO_CD_MUTE,  1 },
    { TOPO_CD_MUTE,  1, TOPO_MASTER_VOL, 4 },

    // Master Vol → Master Mute → Speaker out
    { TOPO_MASTER_VOL,  1, TOPO_MASTER_MUTE, 1 },
    { TOPO_MASTER_MUTE, 1, PCFILTER_NODE, 4  },

    // S/PDIF path: Master → SPDIF enable → S/PDIF out
    { TOPO_MASTER_MUTE, 2,  TOPO_SPDIF_ENABLE, 1 },
    { TOPO_SPDIF_ENABLE, 1, PCFILTER_NODE, 5 },

    // Record path: MUX → RecordVol → capture bridge
    { PCFILTER_NODE, 1, TOPO_RECORD_MUX, 1 },  // Mic → MUX
    { PCFILTER_NODE, 2, TOPO_RECORD_MUX, 2 },  // Line → MUX
    { PCFILTER_NODE, 3, TOPO_RECORD_MUX, 3 },  // CD → MUX
    { TOPO_RECORD_MUX, 1, TOPO_RECORD_VOL, 1 },
    { TOPO_RECORD_VOL, 1, PCFILTER_NODE, 6 },
};

// ─── Topology Filter Descriptor ───────────────────────────────────────────

PCFILTER_DESCRIPTOR g_TopoFilterDescriptor =
{
    0,
    nullptr,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_TopoPins),
    g_TopoPins,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(g_TopoNodes),
    g_TopoNodes,
    sizeof(PCCONNECTION_DESCRIPTOR),
    SIZEOF_ARRAY(g_TopoConnections),
    g_TopoConnections,
    0
};
