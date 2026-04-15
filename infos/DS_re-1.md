## Phase 1 – Inventory and Classification

### 1. Findings

| File | Probable role | Relevance | Contains |
|------|---------------|-----------|----------|
| `sha256.csv` | Integrity manifest | Low | List of driver package files with SHA256 hashes |
| `CRLP3D.TXT` | User documentation | Low | Usage text for CRL positional audio |
| `MAESTRO.COM` | DOS utility | **High** | Binary containing PCI config, I/O port writes, IRQ/DMA setup; includes strings identifying chip variants and legacy config |
| `SETUP.INI` | Installer metadata | Low | Application name (`Maestro-2E`), free disk space requirement |
| `SETUP.INS` | InstallShield script (compiled) | Medium | Installation actions (file copy, registry, INF execution) – obfuscated but inferable from strings |
| `SETUP.ISS` | Silent install response | Low | Default dialog answers |
| `SETUP.LID` | Language selection | Low | `0009` = English |
| `maestro2em.c` | Partial Linux driver | **High** | Register definitions, DMA pool management, AC'97 ops, ISIS firmware loading sequence; incomplete PCM implementation |
| `pbmaestro2.pdf` | Product brief | **High** | Pinout, feature list, AC'97 interface, PCI config, GPIO assignments, hardware volume control, MPU‑401, game port, I²S/Zoom Video |

**Additional files listed in `sha256.csv` (not provided in content):**

| Filename | Probable role | Relevance |
|----------|---------------|-----------|
| `MAESTRO.INF` | Device installation information | **Critical** – defines hardware IDs, resource assignments, driver layering |
| `MSTROWT.INF` | Wavetable synthesizer INF | **High** – wavetable configuration |
| `ES197X.VXD` | Audio VxD (kernel driver) | **Critical** – core audio driver for Windows 9x |
| `ESENUM.VXD` / `ESMGR.VXD` | VxD enumerator / manager | High – PCI enumeration and resource arbitration |
| `MSTR401.VXD` / `MSTR401.DRV` | MIDI UART driver | High – MPU‑401 emulation |
| `AECU.SYS` | Possibly AC‑97 / Codec Utility | Medium |
| `CRL197X.VXD` / `CRLDS3D.VXD` | Positional 3D audio VxDs | Medium |
| `PLAT2MEG.DLL` / `PLAT4MEG.DLL` + `.IMG` | Wavetable sample banks | High – synthesizer data |
| `DATA1.CAB`, `_SYS1.CAB`, `_USER1.CAB` | Compressed installation files | Contains all binaries listed above |
| `ESSETUP.EXE` | Setup launcher | Low |
| `MAESTRO.CAT` / `MSTROWT.CAT` | Security catalogs | Low |

### 2. Evidence

- **`MAESTRO.COM`** contains strings: `"ESS PCI AUDIO"`, `"Maestro 1"`, `"Maestro 2"`, `"Canyon-3D"`, `"Maestro-2E"`, `"Solo"`, `"Allegro 1"`, `"Maestro 3"`, `"Maestro 3P"`. It includes I/O port read/write loops (e.g., `outw`/`inw` at `0x2`/`0x0` index/data pair) and PCI configuration space access (`0xCF8`/`0xCFC`). The code references `BLASTER` environment variable and Sound Blaster emulation settings. This confirms the utility configures legacy DOS compatibility and can be used to extract register base addresses and resource assignments.

- **`maestro2em.c`** defines register offsets consistent with ESS Maestro‑2:
  - Index/data pair at `0x02` / `0x00`
  - AC'97 registers at `0x30` / `0x32`
  - ASSP control ports at `0xA2`, `0xA4`, `0xA6`
  - Host IRQ status at `0x18`
  - GPIO ports at `0x60`, `0x64`, `0x68`
  - ISIS/SAM control via `0x44`/`0x46`
  These match the pin descriptions in **`pbmaestro2.pdf`** (e.g., PCI BAR0 I/O space, AC'97 link, MPU‑401 at `0x98`). The code also contains a firmware upload sequence for an external DSP ("ISIS SAM") using a binary blob (`pci64.bin`), not present in the provided files.

- **`pbmaestro2.pdf`** confirms:
  - PCI 2.1 bus master with scatter‑gather support.
  - Dual AC'97 codec interfaces.
  - Hardware volume control via GPIO pins (VOLDN/VOLUP).
  - MPU‑401 MIDI UART.
  - Game port.
  - I²S / Zoom Video interface (likely unused on Maxi Studio ISIS).
  - ACPI power management (D0–D3).
  - WaveCache technology using host memory for wavetable samples.

### 3. Interpretation

The driver stack is a typical Windows 9x VxD‑based audio solution:

- **Kernel‑mode VxDs**: `ES197X.VXD` (primary audio), `ESENUM.VXD` (PCI enumeration), `ESMGR.VXD` (resource manager), `MSTR401.VXD` (MIDI). These handle hardware access, interrupt handling, DMA management, and mixing.
- **User‑mode DRV/DLL**: `ES197X.DRV` provides the MME/Wave API interface; `A3D.DLL`/`AUDIO3D.DLL` for 3D audio extensions.
- **Wavetable synthesis**: `PLAT2MEG.DLL`/`IMG` are sample banks used by the hardware wavetable engine (64‑voice wavelet synthesizer) or by a software synthesizer that feeds the hardware via WaveCache.
- **ISIS/SAM DSP**: The Guillemot Maxi Studio ISIS adds an external DSP (likely for additional I/O or effects) that requires firmware upload. The Linux code includes a sequence to load `pci64.bin`. The Windows driver probably contains a similar firmware image embedded in a resource or a separate file (e.g., inside `DATA1.CAB`).

The `MAESTRO.INF` file would reveal:
- Hardware IDs (e.g., `PCI\VEN_125D&DEV_1978` for Maestro‑2E).
- Registry settings for I/O ranges, IRQ steering, and DMA policy (DDMA, PC/PCI, TDMA).
- Driver service installation for both VxD and WDM (if dual‑mode).

### 4. Proposed Implementation Impact

- **Highest priority for extraction**: `MAESTRO.INF`, `ES197X.VXD`, and the wavetable image files. Without these, reverse engineering of the exact register programming for playback/capture is difficult.
- **`MAESTRO.COM`** can be disassembled (e.g., with IDA or Ghidra) to obtain:
  - I/O port base address determination logic.
  - PCI configuration space accesses (vendor/device check, BAR sizing).
  - Legacy resource assignment (I/O, IRQ, DMA) for DOS compatibility.
  - Possible initialisation sequence for the AC'97 codec and APU.
- **`maestro2em.c`** provides a solid foundation for the ALSA driver structure (PCI probe, DMA pool, AC'97 ops, firmware loading). However, its PCM implementation is skeletal and must be completed based on hardware knowledge.

### 5. Open Questions

- Are the VxD and INF files available for analysis? If not, we must rely on disassembly of `MAESTRO.COM` and inference from similar ESS Maestro drivers (e.g., Linux `snd-es1968` which is for a related chip).
- Does the Maxi Studio ISIS use the secondary AC'97 interface or the I²S port for additional I/O?
- What is the exact firmware file (`pci64.bin`) and where is it located in the Windows package?
- How are the wavetable samples loaded into host memory and referenced by the hardware?

### 6. Confidence Rating

- **File classification**: High (clear naming conventions and known Windows driver structure).
- **Role of each binary**: Medium‑High (can be cross‑referenced with standard ESS driver packages from the era).

---

## Phase 2 — Windows Driver Architecture Reconstruction (Preliminary)

*Note: Full reconstruction requires disassembly of `ES197X.VXD` and analysis of `MAESTRO.INF`. The following is based on known Windows 9x audio driver patterns and the limited evidence available.*

### 1. Findings

| Component | Entry Point | Responsibility | Certainty |
|-----------|-------------|----------------|-----------|
| `ESENUM.VXD` | `Device_Init` | PCI device enumeration, resource assignment (I/O, IRQ) | High (inferred) |
| `ESMGR.VXD` | `Device_Init` | Central resource manager for multiple audio functions (wave, MIDI, mixer) | High (inferred) |
| `ES197X.VXD` | `WAVE_Open`, `WAVE_Close`, `WAVE_Write`, interrupt handler | Wave audio playback/recording, DMA buffer management, hardware mixer control | High |
| `MSTR401.VXD` | `MIDI_Open`, `MIDI_Write` | MPU‑401 UART emulation, MIDI data transmission | High |
| `MAESTRO.INF` | – | Maps PCI device ID to driver services, configures legacy resources (I/O, IRQ, DMA) for DOS box compatibility | Critical |
| Installer (`SETUP.EXE` + InstallShield) | – | Copies files, runs `MAESTRO.COM` to set up DOS environment, registers VxDs in `system.ini` | Medium |

**Initialization sequence (inferred):**
1. PCI bus scan finds `VEN_125D&DEV_1978`.
2. `ESENUM.VXD` loads and allocates I/O and IRQ resources (possibly using PCI BIOS).
3. `ESMGR.VXD` initialises the hardware: reset AC'97, configure APU, set up DMA engine.
4. If DOS compatibility is enabled, `MAESTRO.COM` runs from `autoexec.bat` to set `BLASTER` variable and program legacy registers (SB Pro emulation).

**Playback path (inferred):**
- Application writes PCM data to a circular buffer in host memory.
- VxD programs the APU (Audio Processing Unit) descriptors to point to this buffer, using scatter‑gather DMA.
- Hardware reads data via PCI bus mastering, mixes with other streams, and sends to AC'97 codec.
- Interrupts are generated at each period (fragment) boundary; the VxD updates the `dwCurrent` pointer and signals the application.

### 2. Evidence

- **`MAESTRO.COM`** contains code that writes to I/O ports `0x2`/`0x0` (index/data) and reads PCI config space. It also sets up the `BLASTER` variable with `A220 I5 D1 T4` style parameters. This mirrors the behaviour of Sound Blaster emulation on ESS chips.
- **`pbmaestro2.pdf`** states the chip supports "Distributed DMA protocol, PC/PCI DMA, Compaq style one‑signal SERIRQ#, and Transparent DMA" – these are the legacy DMA compatibility modes.
- **Strings in `MAESTRO.COM`** like `"SBPro Disabled"`, `"SB IO=220h"`, `"IRQ=5"`, `"DMA=1"` confirm the utility configures SB emulation.
- **Linux `maestro2em.c`** shows the AC'97 codec is accessed through a separate register window (`0x30`/`0x32`) and that the APU is controlled via the index/data pair.

### 3. Interpretation

The Windows driver follows the classic ESS Maestro‑2 architecture:
- The chip acts primarily as a PCI bus master with multiple DMA engines (one per audio stream).
- A "WaveCache" mechanism allows the driver to allocate host memory for wavetable samples and PCM buffers; the hardware accesses this memory via scatter‑gather lists.
- The external ISIS/SAM DSP on the Guillemot card is likely used to expand I/O (e.g., additional analog inputs/outputs) or to provide hardware effects. Its firmware must be uploaded after power‑on.

**Missing critical pieces:**
- Exact register programming for APU channel setup (descriptor format, buffer base address, period size).
- Interrupt routing: how does the chip signal period completion? (The Linux code uses `ESM_HIRQ_DSIE` – Distributed DMA interrupt enable.)
- The role of the ISIS DSP in the audio path (is it a coprocessor or a separate device?).

### 4. Proposed Implementation Impact

- The ALSA driver must replicate the Windows driver's resource acquisition:
  - PCI BAR0 I/O region (confirmed at offset `0x10`).
  - IRQ line (from PCI config).
- It must initialise the AC'97 codec and the APU.
- It must implement the same DMA descriptor scheme (likely similar to the es1968 driver, which uses a ring of descriptors in host memory).
- The ISIS firmware upload sequence from `maestro2em.c` is likely correct and should be retained.

### 5. Open Questions

- Where is the firmware image (`pci64.bin`) stored in the Windows driver package? (Probably inside `DATA1.CAB` or as a resource in `ES197X.VXD`.)
- Does the driver use MSVAD (Microsoft Virtual Audio Device) or a custom VxD for mixing?
- How are the wavetable samples loaded into WaveCache? (They may be in a proprietary format inside `.IMG` files.)

### 6. Confidence Rating

- **Overall architecture**: High (matches known ESS designs and the Linux code).
- **Detailed register programming**: Low (requires disassembly of VxD or exhaustive testing of hardware).

---

## Next Steps (Phases 3–7)

Given the current evidence, the next logical step is to obtain disassembly of `ES197X.VXD` and the content of `MAESTRO.INF`. This will provide the precise register definitions and resource assignments needed to complete Phases 3 (hardware model) and 4 (cross‑correlation).

**Recommended immediate actions:**
1. Extract `MAESTRO.INF` and `ES197X.VXD` from the provided `sha256.csv` (if the actual files are available).
2. Disassemble `ES197X.VXD` using a tool like IDA Pro or Ghidra (VxD format is well‑documented).
3. Extract firmware image from the Windows driver package (search for `pci64.bin` or similar magic bytes).
4. Use `MAESTRO.COM` as a secondary source for I/O port usage and PCI config.

Once this additional evidence is available, a complete hardware model and ALSA mapping can be constructed with high confidence.

---

## Summary

### What is known

- The Guillemot Maxi Studio ISIS is based on the ESS Maestro‑2 PCI audio accelerator (device ID `0x1978`).
- The Windows driver uses a VxD stack with separate components for wave, MIDI, and mixer.
- The hardware includes an AC'97 codec, MPU‑401 UART, game port, and an external DSP (ISIS) requiring firmware.
- The Linux `maestro2em.c` provides a partial ALSA driver skeleton with correct register offsets and DMA pool management.

### What is likely

- The chip uses a descriptor‑based scatter‑gather DMA engine for audio streams.
- The wavetable synthesizer is implemented in hardware using WaveCache to access sample data from host memory.
- The `MAESTRO.INF` file contains the resource assignments and driver layering information.

### What remains unknown

- Exact descriptor format and APU programming sequence.
- Interrupt handling details (which bits correspond to which streams).
- Firmware image location and format.
- Role of the ISIS DSP in the audio signal chain.

### Next best step

Acquire and disassemble `ES197X.VXD` and `MAESTRO.INF`. This will unlock Phase 3 and 4 and allow a complete driver reconstruction.
