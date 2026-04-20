// SPDX-License-Identifier: GPL-2.0-only
/*
 * minwave.h  —  WaveRT miniport for Maestro-2E
 *               IMiniportWaveRT + IMiniportWaveRTStream + Notification
 *               Exposes WASAPI exclusive-mode and shared-mode access.
 */
#pragma once
#include "hw.h"
#include "adapter.h"

// {A4E5E2E0-1978-125D-0010-14AF0000BEEF}
DEFINE_GUID(CLSID_MiniportWaveRT_Maestro,
    0xa4e5e2e0, 0x1978, 0x125d,
    0x00, 0x10, 0x14, 0xaf, 0x00, 0x00, 0xbe, 0xef);

// Pin IDs (must match table in tables.cpp)
#define PINID_RENDER_SINK       0   // host → DAC
#define PINID_CAPTURE_SOURCE    1   // ADC → host
#define PIN_COUNT               2

// Supported formats
static KSDATAFORMAT_WAVEFORMATEXTENSIBLE g_Formats[] =
{
    // 48 kHz stereo 16-bit  (primary — WASAPI-native)
    {
        { sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
          0, 0, 0,
          DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_EXTENSIBLE),
          STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
          STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
          STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
        { { WAVE_FORMAT_EXTENSIBLE, 2, 48000, 192000, 4, 16, 22 },
          16, KSAUDIO_SPEAKER_STEREO,
          DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_PCM) }
    },
    // 44.1 kHz stereo 16-bit
    {
        { sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
          0, 0, 0,
          DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_EXTENSIBLE),
          STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
          STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
          STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
        { { WAVE_FORMAT_EXTENSIBLE, 2, 44100, 176400, 4, 16, 22 },
          16, KSAUDIO_SPEAKER_STEREO,
          DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_PCM) }
    },
    // 48 kHz stereo 24-bit  (for PCM1728E ext-box DAC)
    {
        { sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
          0, 0, 0,
          DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_EXTENSIBLE),
          STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
          STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
          STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
        { { WAVE_FORMAT_EXTENSIBLE, 2, 48000, 288000, 6, 24, 22 },
          24, KSAUDIO_SPEAKER_STEREO,
          DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_PCM) }
    },
    // 8000 Hz mono 16-bit   (low rate)
    {
        { sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
          0, 0, 0,
          DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_EXTENSIBLE),
          STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
          STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
          STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
        { { WAVE_FORMAT_EXTENSIBLE, 1, 8000, 16000, 2, 16, 22 },
          16, KSAUDIO_SPEAKER_MONO,
          DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_PCM) }
    },
};

// Forward
class CMaestroWaveRTStream;
class CMaestroWaveRT;

NTSTATUS NewMiniportWaveRT_Maestro(
    _Out_ PUNKNOWN*  Unknown,
    _In_  REFCLSID   ClassId,
    _In_  PUNKNOWN   OuterUnknown,
    _In_  POOL_TYPE  PoolType);
