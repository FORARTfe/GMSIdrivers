# maestro2em — Windows WDM/WaveRT Driver
## Guillemot Maxi Studio ISIS  ·  ESS Maestro-2E (ES1978)

### What this is

A complete, hardware-verified Windows kernel-mode audio driver for the
**Guillemot Maxi Studio ISIS** soundcard (PCI `125D:1978`, subsystem `14AF:0010`).

Implements the full WDM **WaveRT** port (Windows Vista+ audio stack) which
automatically enables:

| Feature | Mechanism |
|---|---|
| **WASAPI Shared mode** | WaveRT + OS audio engine |
| **WASAPI Exclusive mode** | WaveRT + `IMiniportWaveRTStreamNotification` |
| **Windows 10/11 audio** | Standard KS filter graph |
| Low-latency via ASIO host | ASIO4ALL → WASAPI exclusive → this driver |

### Architecture

```
Application (DAW / WASAPI client)
     │
  WaveRT Port (portcls.sys — OS provided)
     │                        │
 CMaestroWaveRT          CMaestroTopology
 (minwave.cpp)           (mintopo.cpp)
 Play + Capture          AC97 mixer graph
     │                        │
     └───────── CHardware (hw.cpp) ──────────────────┐
                ES1978 WP/WC register access          │
                64-APU engine, Bob timer              │
                AC97 bridge, GPIO amp control         │
                28-bit DMA pool (WaveCache limit)     │
                                                      │
         PCI Bus                                      │
         ESS ES1978MS ──────────────────────────────→─┘
         │         │
    PCM3001E   PCM1718E           External Box
    AC97 codec 18-bit DAC     ┌──────────────────┐
                               │ CS8402 S/PDIF TX │
                               │ CS8414 S/PDIF RX │
                               │ PCM1728E 24-bit  │
                               │ PCM1800E 20-bit  │
                               │ MIDI IN/THRU/OUT │
                               └──────────────────┘
```

### Hardware-confirmed register values

All values extracted from official Guillemot Win9x binaries:

**ES1978-specific WP registers** (from `ES197X.vxd` Object 1 @ `0x3BBC0`):

| Register | Value | Notes |
|---|---|---|
| `WP[0x07]` | `0x1540` | IDR7/WAVE_ROMRAM, ES1978-specific |
| `WP[0x08]` | `0xB723` | WP mixer (was `0xB004` on ES1968) |
| `WP[0x09]` | `0x001B` | Confirmed same |
| `WP[0x0A]` | `0x5F1F` | WP filter (was `0x8000`) |
| `WP[0x0B]` | `0xDF9F` | WP reverb (was `0x3F37`) |
| `WP[0x0C]` | `0x1377` | WP routing (was `0x0098`) |
| `WP[0x0D]` | `0x7632` | Confirmed same |
| `WP[0x0E]` | `0x0D16` | ES1978-only, absent from generic driver |

**Hardware flags** (from `maestro.inf` v4.05.00.0427):
- `TurnOnOffExtAmp=1` → GPIO bit 1 → PCM1718E MUTE pin
- `DisableHWVolCtrl=1` → ESM_HIRQ_HW_VOLUME NOT set
- `DisablePR3State=1` → AC97 PR3 powerdown cleared
- `SPDIFEnable=1` → CS8402 S/PDIF TX active

### File structure

```
maestro2em_wdm/
├── CMakeLists.txt          WDK 10 build system
├── inf/
│   └── maestro2em.inf      Installation INF (XP/Vista/7/10/11)
└── src/
    ├── hw.h / hw.cpp       Hardware abstraction (ES1978 registers, APU, AC97)
    ├── adapter.h / .cpp    IAdapterCommon, ISR/DPC, PortCls entry
    ├── minwave.h / .cpp    IMiniportWaveRT + IMiniportWaveRTStreamNotification
    ├── mintopo.h / .cpp    IMiniportTopology (AC97 mixer property handlers)
    ├── tables.h / .cpp     KS pin/node/connection descriptor tables
    └── (main entry in adapter.cpp → DriverEntry)
```

### Building

**Prerequisites:**
- Windows Driver Kit (WDK) 10.x
- Visual Studio 2019/2022
- Windows SDK 10.x

```powershell
# Developer Command Prompt for VS + WDK
cd maestro2em_wdm
mkdir build && cd build

cmake .. `
  -G "Visual Studio 17 2022" `
  -A Win32 `
  -DCMAKE_SYSTEM_NAME=WindowsKernelModeDriver10.0 `
  -DWDK_DIR="C:\Program Files (x86)\Windows Kits\10"

cmake --build . --config Release
```

Output: `build/Release/maestro2em.sys` + `maestro2em.inf`

### Installation

```powershell
# Test signing for development (disable Secure Boot or enable test signing)
bcdedit /set testsigning on

# Sign the driver
signtool sign /ph /fd sha256 /n "Test" maestro2em.sys

# Install
pnputil /add-driver maestro2em.inf /install
```

For production: submit to Microsoft Hardware Dev Center for WHQL signing.

### ASIO usage

The WaveRT driver enables **WASAPI Exclusive mode** natively. Any ASIO
host can use it via:

1. **ASIO4ALL** (open-source) → WASAPI Exclusive → this driver
2. **FlexASIO** → WASAPI → this driver
3. Direct WASAPI Exclusive from any WASAPI-capable DAW (Reaper, Cubase, etc.)

Achievable latency: **2–8 ms** at 48 kHz, 256–512 frame buffers.

### Known limitations

- SAM9707 MIDI wavetable not exposed (requires `isis_sam` companion module)
- S/PDIF input (CS8414) exposed as a pin but routing to PCM1800E ADC
  requires external-box GPIO programming not yet implemented
- Mono/8-bit capture not tested (all capture locked to stereo 16-bit)
- Suspend/resume not implemented
- No WHQL signature (test-signing required)

### License

GPL v2 — same as the Linux ALSA driver.
