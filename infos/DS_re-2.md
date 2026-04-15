## A. Strongly Supported Facts (direct evidence)

| Fact | Evidence Source |
|------|-----------------|
| The card identifies as PCI vendor `0x125D`, device `0x1978` (ESS Maestro-2E) | `maestro2em.c` PCI ID table; consistent with ESS product line |
| The device uses PCI BAR0 for I/O space access | `maestro2em.c` line `MAESTRO_BAR0 0`; `pbmaestro2.pdf` pin table shows PCI I/O pins |
| Index/data register pair is at offsets `0x02` (index) and `0x00` (data) within BAR0 | `maestro2em.c` defines `ESM_INDEX 0x02`, `ESM_DATA 0x00` |
| AC'97 codec interface is at offsets `0x30` (index) and `0x32` (data) | `maestro2em.c` defines `ESM_AC97_INDEX 0x30`, `ESM_AC97_DATA 0x32` |
| Host IRQ status register is at offset `0x18` | `maestro2em.c` defines `ESM_PORT_HOST_IRQ 0x18` |
| MPU-401 UART is at offset `0x98` | `maestro2em.c` defines `ESM_MPU401_PORT 0x98`; `pbmaestro2.pdf` lists TXD/RXD pins |
| GPIO ports are at `0x60` (data), `0x64` (mask), `0x68` (direction) | `maestro2em.c` defines `ESM_GPIO_DATA 0x60` etc. |
| The DOS utility `MAESTRO.COM` manipulates `BLASTER` environment variable and I/O ports | Strings in binary: `"BLASTER=A220 D1 I10 T4"`, `"SB IO=220h"`, `"IRQ=5"` |
| The card requires an external firmware blob for ISIS/SAM DSP operation | `maestro2em.c` contains `maestro_upload_firmware()` that loads `pci64.bin` |
| The SAM firmware upload uses ISIS_DATA (`0x46`) and ISIS_ADDRESS (`0x44`) ports | `maestro2em.c` defines these offsets and uses them in burst-write functions |
| The card has a 64-voice wavetable synthesizer | `pbmaestro2.pdf` page 1, first paragraph |
| The chip supports scatter-gather DMA (PCI bus master) | `pbmaestro2.pdf` page 1 "PCI 2.1 bus master with scatter/gather support" |
| The chip supports multiple legacy DMA compatibility modes (DDMA, PC/PCI, TDMA) | `pbmaestro2.pdf` page 1; `MAESTRO.COM` strings refer to "DDMA", "TDMA", "PCPCI" |
| The Windows driver package includes VxD files (`ES197X.VXD`, `ESENUM.VXD`, etc.) | `sha256.csv` lists them |
| The `maestro2em.c` DMA pool implementation matches the es1968 ALSA driver's reservation strategy | Code comment explicitly states "es1968 exact port" |

## B. Weakly Supported Inferences (logical but unconfirmed)

| Inference | Basis | Weakness |
|-----------|-------|----------|
| The Windows VxD stack uses `ES197X.VXD` as primary wave driver, `ESENUM.VXD` as enumerator, `ESMGR.VXD` as resource manager | Typical ESS driver structure from the era; file naming patterns | No actual VxD analysis performed; roles could be merged differently |
| The ISIS DSP is a separate device (not integrated into Maestro-2) that communicates via the ISIS_ADDRESS/DATA ports | `maestro2em.c` treats it as a distinct entity requiring firmware | No datasheet for ISIS DSP; could be a misidentified secondary codec or GPIO extender |
| The wavetable samples are loaded into host memory and accessed via WaveCache | `pbmaestro2.pdf` describes WaveCache; `.IMG` files present | No confirmation that `.IMG` files are WaveCache-compatible; could be proprietary format |
| The MPU-401 interface is fully UART-mode only (no intelligent mode) | `maestro2em.c` does not implement MPU-401 IRQ handling; typical for ESS | Could support intelligent mode with firmware |
| The AC'97 codec is a standard AC'97 revision 2.x | `maestro2em.c` uses standard AC'97 read/write ops | No codec ID readback implemented; could be ESS PT-101 or other |
| The `MAESTRO.COM` utility sets up SB Pro emulation registers that persist across warm boot | Typical DOS TSR behavior | Binary not disassembled; may only configure PCI config space, not I/O registers |
| The driver uses the same APU descriptor format as the original es1968 driver | `maestro2em.c` comment "TODO: Here you must program WaveCache/APU descriptors" | No evidence that descriptor layout is identical to es1968 (Maestro-1) |
| The firmware file `pci64.bin` is present in the Windows driver package | `maestro2em.c` expects it; common practice to embed firmware in CAB files | Not found in provided file list; could be named differently |

## C. Speculative Assumptions (unsupported by current evidence)

| Assumption | Why it's speculative |
|------------|----------------------|
| The ISIS/SAM DSP handles additional I/O (e.g., more analog inputs) | No datasheet; `maestro2em.c` does not expose extra PCM devices |
| The card has 8 channels of playback and 8 channels of capture | `maestro2em.c` sets `channels_max = 8` but provides no hardware justification |
| The wavetable `.IMG` files are DLS Level 1 or 2 compliant | DLS is mentioned in `pbmaestro2.pdf` but no proof `.IMG` is DLS |
| The interrupt handler uses `ESM_HIRQ_DSIE` for period completion | `maestro2em.c` includes that bit in the IRQ handler skeleton, but no tested hardware |
| The `MAESTRO.COM` DOS utility is required to enable SB emulation on Linux | Linux drivers typically handle this in kernel; the DOS utility is for pure DOS environment |
| The card supports full-duplex operation | `maestro2em.c` creates separate playback and capture PCMs, but no hardware confirmation |
| The GPIO pins control hardware volume up/down buttons | `pbmaestro2.pdf` lists VOLDN/VOLUP on GPIO4/5; no evidence the Maxi Studio ISIS uses them |
| The firmware upload sequence in `maestro2em.c` is correct for all card revisions | The sequence is derived from maxiinit 0.2.1, which may be incomplete or version-specific |

## D. Potential Errors in Previous Analysis

| Claim in previous analysis | Likely error | Corrective action |
|---------------------------|--------------|-------------------|
| "The Windows driver stack is a typical VxD‑based solution" | Over-generalization: The driver could be WDM on Windows 98/ME (dual-mode INF) | Need to examine `MAESTRO.INF` to see if `NTMP` (WDM) sections exist |
| "`ESENUM.VXD` loads and allocates I/O and IRQ resources" | VxD enumeration is done by the IOS subsystem; `ESENUM.VXD` is likely a bus enumerator for ISA PnP cards, not PCI | The PCI device is enumerated by PCI bus driver; `ESENUM` may handle the MPU-401 or game port as separate logical devices |
| "The ISIS/SAM DSP on the Guillemot card is likely used to expand I/O" | Could be a red herring: "ISIS" may simply be Guillemot's marketing name for the Maestro-2E chip, not a separate DSP | The `maestro2em.c` file treats ISIS as a SAM (Simple Audio Machine?) coprocessor inside the Maestro chip; the `pci64.bin` may be for the APU itself |
| "The wavetable synthesizer is implemented in hardware using WaveCache" | WaveCache is a DMA caching scheme, not the synthesizer engine; the 64-voice engine is on-chip | The `.IMG` files could be for a *software* wavetable that uses the hardware mixer, not the hardware synth |
| "The ALSA driver must implement the same descriptor scheme (likely similar to es1968)" | The es1968 is Maestro-1/2, but the descriptor format may differ for Maestro-2E (ES1978) | Need to obtain actual descriptor programming from VxD disassembly |
| "The driver should be implemented as a single PCM device with 8 channels" | The card may expose multiple independent stereo pairs (e.g., Front, Rear, Center/LFE) due to AC'97 2.x | AC'97 codec capabilities unknown; mixer routing may reveal multiple outputs |
| False equivalence: "The Linux code's AC'97 ops are correct" | The AC'97 read/write in `maestro2em.c` uses a busy-wait loop without timeout protection; this may hang on real hardware | The original es1968 driver uses `snd_maestro_ac97_wait()` with proper timeout |

## E. Revised Implementation Priority

### Safe to Implement Now (high confidence, based on strong facts)

| Component | Rationale |
|-----------|-----------|
| PCI device ID table with `PCI_DEVICE(0x125D, 0x1978)` | Confirmed by multiple sources |
| BAR0 I/O region request | Confirmed by `pbmaestro2.pdf` pinout |
| Basic index/data register read/write wrappers | Offsets confirmed |
| AC'97 bus and codec attachment using standard ALSA AC'97 API | AC'97 interface location confirmed; codec type will be auto-detected |
| DMA pool allocation using `snd_dma_*_reserved` pattern from `maestro2em.c` | Code is a direct port of working es1968 driver |
| MPU-401 rawmidi device at port offset `0x98` | Pinout and Linux code agree |
| Firmware request infrastructure (request_firmware) | Necessary for ISIS functionality; stub can be used until firmware is found |

### Implement Only Behind Abstraction Barriers (uncertain behavior)

| Component | Barrier |
|-----------|---------|
| APU descriptor format and programming | Wrap in `maestro_apu_setup()` with placeholder implementation |
| Interrupt handler logic (which bits map to which streams) | Use `maestro_irq_dispatch()` that reads status register and calls per-stream callbacks; actual mapping TBD |
| PCM hardware parameter constraints (channels, rates) | Expose conservative defaults (stereo, 48kHz); re-evaluate after testing |
| WaveCache wavetable loading | Isolate in `maestro_wavetable.c`; not required for basic PCM |
| GPIO / hardware volume control | Abstract behind mixer controls with `-ENOTSUPP` until proven |
| Playback/capture trigger (start/stop sequence) | Use `maestro_stream_start()` / `maestro_stream_stop()` that currently no-ops |

### Do Not Implement Yet (requires binary confirmation)

| Item | Required evidence |
|------|-------------------|
| PCM period interrupt handling | Disassembly of `ES197X.VXD` interrupt handler or logic analyzer trace of IRQ line |
| APU descriptor layout | VxD code that writes to descriptor area; or known-working es1968 descriptor layout (but confirm compatibility with ES1978) |
| Firmware image extraction | Locate `pci64.bin` (or equivalent) in Windows driver package; verify magic bytes / checksum |
| Mixer routing (which AC'97 controls map to which physical outputs) | Need to read AC'97 codec ID and consult datasheet; or inspect Windows mixer control layout in registry |
| Multi-channel support (beyond stereo) | Determine if card exposes multiple AC'97 slots or uses secondary codec interface |
| Power management (D0-D3) | PCI config space capabilities; VxD power handler analysis |
| Wavetable synth integration | Determine if `.IMG` files are loaded by VxD or by a user-mode DLL |

## F. Evidence Still Required (minimum for production driver)

| Priority | Evidence | How to obtain |
|----------|----------|---------------|
| Critical | `MAESTRO.INF` file | Extract from driver package |
| Critical | Disassembly of `ES197X.VXD` (wave streaming and interrupt handler) | IDA Pro or Ghidra with VxD loader |
| High | Firmware image `pci64.bin` (or equivalent) | Search CAB files; if not present, card may not require external firmware (ISIS could be on-chip) |
| High | AC'97 codec ID (read from codec register 0x00) | Test on real hardware (if available) or from Windows registry dump |
| Medium | Descriptor area base address and format | Compare with es1968 driver; if different, extract from VxD |
| Medium | Interrupt status bit definitions | VxD interrupt handler will read a status register and branch; trace the bit tests |
| Low | Game port / joystick implementation | Not essential for audio driver; can be separate module |

**Confidence in current state:**
- Hardware identification and basic I/O layout: **High**
- DMA and buffer management: **Medium** (es1968 pattern likely correct)
- PCM streaming details: **Low** (requires VxD analysis)
- Mixer and controls: **Low** (depends on AC'97 codec capabilities)
