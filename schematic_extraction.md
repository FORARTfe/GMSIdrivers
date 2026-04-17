# Guillemot Maxi Studio ISIS Schematic Extraction (v0.7)

This document contains a structured technical extraction from the scanned schematic (version 0.7 by Jimmy Le Rhun, GPL, June 2003) for the **Guillemot Maxi Studio ISIS** PCI audio card. This information is intended to aid in reverse-engineering the card's hardware for driver development.

The schematic details the three main components of the system:
1.  **Mainboard Part 1: Maestro** (ESS ES1978MS / Maestro-2EM subsystem)
2.  **Mainboard Part 2: Dream** (SAM9707 wavetable synth subsystem)
3.  **Daughter Board** and **External Box** (connectors, S/PDIF, MIDI, and additional audio I/O)

---

## 1. Component Inventory

| Reference | Part Number | Function | Key Pins | Sources |
| :--- | :--- | :--- | :--- | :--- |
| **IC15** | AT93C46 | EEPROM | UCC, GND, DO, DI, SK, CS, ORS | [360, 389], [390] |
| **IC15** (page 2) | SRAM | SRAM | A0-A19, D0-D7, WE, CE | [441, 461] |
| **PCM1718E** | PCM1718E | 18-Bit Stereo Audio DAC | BCKIN, DIN, LRCIN, XTI, XTO, ZERO, FOR, DME, VCC, VDO, MUTE, RSTB, CLKO, D/C R, D/C L, OUTL, OUTR | [636] |
| **SAM9787** | SAM9787 | Dream Sound Synthesis Chip | A0-A22, D0-D15, CLK, OSC, CE, WE, RD, BOOT, RUN, RESET | [653] |
| **MK1413** | MK1413 | Low Power Audio Clock Source | CLK, GND, UCC, VCC | [660] |
| **PCM3881E** | PCM3881E | Audio Codec (?) | DIN, DOUT, INR, OUTR, INL, OUTL, XTI, XTO, FMT0-FMT2, LACIN, BCKIN, RSTB, CPL, CLKIO, AGND2, UCC2 | [704] |
| **CS8414** | CS8414 | 96 kHz Digital Audio Receiver | ROXP, PACK, RXN, SCK, FSYNC, SDATA, VERF, ERF, FILT, FCK, SEL, VDD, DGND, UCC, AGND | [872] |
| **CS8402** | CS8402 | Digital Audio Transmitter | MCK, TXP, SCK, FSYNC, SDATA, RST, FCC0-2, CBL, PRO, VDD, GND | [903] |
| **PCM1728E** | PCM1728E | 24-Bit, 96 kHz Stereo Audio DAC | DIN, VOUT R, VOUT L, SYSCLK, XTI, XTO, BCK, LRCIN, BEKIN, FOR, DME, ZERO, CLKO, MUTE, RSTB, AGND1, AGND2, VCC1, VCC2 | [938, 971] |
| **PCM1800E** | PCM1800E | 20-Bit Stereo A/D Converter | OUT, INR, INL, RSTB, REFI, REF2, CNR, CPR, MODEL, MODE1, SYSCLK, CPL, FSYNC, UCC, LRCK, VDD, BCK, AGND, DGND | [1033, 1044, 1088, 1096] |
| **26C31** | 26C31 | RS-422 Dual Differential Line Driver | (Standard pinout) | [795, 985] |
| **26C32** | 26C32 | RS-422 Dual Differential Line Receiver | (Standard pinout) | [784, 969, 1049] |
| **74LS74D** | 74LS74D | Dual D-Type Positive-Edge-Triggered Flip-Flop with Preset and Clear | PRE, D, CLK, CLR | [399, 401, 528, 949] |
| **74AC14D** | 74AC14D | Hex Inverting Schmitt Trigger | (Standard pinout) | [706] |
| **74AC88D** | 74AC88D | Logic Gate | (Standard pinout) | [715, 722, 724] |
| **74AC860** | 74AC860 | Logic Gate | (Standard pinout) | [785, 786, 797, 798, 838] |
| **74AC140** | 74AC140 | Logic Gate | (Standard pinout) | [800] |
| **74LS140** | 74LS140 | Logic Gate | (Standard pinout) | [847, 880, 976] |
| **ES1918** | label | Label, possible related chip number | - | [371] |
| **ES197815** | ESS Maestro-2EM | ESS Maestro-2EM PCI Audio Accelerator | - | [391] |

## 2. Netlist Extraction

### Buses

* **ISA BUS**: Appears on Page 1 and Page 2.
* **PCI BUS**: Page 1.
* **SRAM Address Bus (UA0-UA19)**: Page 2.
* **RAM Data Bus (D0-D7)**: Page 2.
* **DRAM Address Bus (HA0-HA10)**: Page 2.
* **DRAM Data Bus (HD0-HD15)**: Page 2.

### Critical Signals

* **MAESTRO_DOUT**: Digital audio output from Maestro subsystem to PCM1718E DAC.
* **MIDI_TXD**: MIDI transmit data signal.
* **MIDI_RXD**: MIDI receive data signal.
* **S/PDIF**: Fiber RX and Fiber TX signals on Page 4.
* **LINE_IN, LINE_OUT**: Audio input and output signals.
* **MIC_IN**: Microphone input signal.
* **AUX_IN**: Auxiliary input signal.
* **CD_IN**: CD audio input signal.
* **SRND_OUT**: Surround sound output signal.

---

## 3. Page-by-Page Breakdown

### Page 1: Maestro

* **Title Block**:
    * **TITLE**: isis2
    * **Date**: 09/06/2003
    * **Sheet**: 1/1
    * **REV**: (empty)
    * **Document Number**: (empty)
    * **Header Notes**: "Incomplete schematic of the Guillemot MAXI Studio ISIS soundcard", "(C) 2003 Jimmy Le Rhun. This document is released under the GPL to help Linux driver development for this card.", "See http://isisalsa.sourceforge.net for details. Any comments, error reports are highly appreciated, please write to jlerhun@wanadoo.fr Version 0.7, 08 June 2003"
* **Major Sections**: ESS Maestro-2EM subsystem, PCI interface, power section, AT93C46 EEPROM.
* **Key Chips**: ESS Maestro-2EM (ES197815), AT93C46 EEPROM (IC15).
* **Buses**: PCI BUS.
* **Power/Ground**: +5U, GND, -12U, +12U, JANTA.

### Page 2: Dream

* **Title Block**:
    * **TITLE**: isis2
    * **Date**: 09/06/2003
    * **Sheet**: 1/1
    * **Header**: Mainboard part 2 : Dream
* **Major Sections**: Dream SAM9707 synthesis subsystem, RAM (SRAM/DRAM) memory, PCM1718E DAC, digital audio section.
* **Key Chips**: Dream SAM9707 (SAM9787), PCM1718E DAC, MK1413 clock source, PCM3881E codec(?).
* **Buses**: SRAM Address Bus, RAM Data Bus, DRAM Address Bus, DRAM Data Bus.
* **Power/Ground**: +5U, GND, UCC.

### Page 3: Daughter Board

* **Title Block**:
    * **TITLE**: isis2
    * **Date**: 09/06/2003
    * **Sheet**: 1/1
    * **Header**: Daughter board
* **Major Sections**: Connectors X7, logic circuitry (RS-422, logic gates), sampling clock generation, GPIO handling for the daughter board.
* **Key Chips**: RS-422 Drivers/Receivers (26C31/32), Logic Gates (74AC14D, 74AC860, 74AC140).
* **Buses**: GPIOB.
* **Power/Ground**: +5U, GND, +12V.

### Page 4: External Box

* **Title Block**:
    * **TITLE**: isis2
    * **Date**: 09/06/2003
    * **Sheet**: 1/1
    * **Header**: External Box
* **Major Sections**: MIDI THRU/OUT ports, S/PDIF interfaces (fiber RX/TX), S/PDIF receiver (CS8414) and transmitter (CS8402), multiple audio DACs and ADCs for line in/out on the external box.
* **Key Chips**: CS8414 S/PDIF receiver (CSA414), CS8402 S/PDIF transmitter (CS8402), PCM1728E DAC, PCM1800E ADC, Logic Gates (74LS140, 74LS740, 74AC880).
* **Power/Ground**: +5U, GND, VDD, DGND, UCC, AGND.

---

## 4. Key Hardware Mappings

### PCI / ISA Bus Connections

* **PCI BUS**: Page 1. Connections like `P\$553 AD 2` [44], `PAD 3` [44] are listed. Further trace would show detailed mapping.
* **ISA BUS**: `ISA BUS` [130, 529]. Traces show typical ISA signals such as `RESET`, `IRQ`, `DMA` pins for the Dream subsystem.

### MIDI TX/RX Paths

* **MIDI_TXD**: [381, 665] - Traces from Maestro to daughter board/external box.
* **MIDI_RXD**: [382, 667] - Traces from external box to Maestro.
* **Fiber RX/TX**: [850, 899] - S/PDIF optical interfaces.

### Audio Routing

* **Digital Audio**: `MAESTRO_DOUT` [710] to `PCM1718E` DAC. `S/PDIF` signals between the mainboard and the external box.
* **Analog Audio**:
    * **Input**: CD_IN, MIC_IN, AUX_IN on mainboard; LINE_IN on mainboard. Multiple LINE_INPUT on external box to PCM1800E ADCs.
    * **Output**: LINE_OUT, Surround OUT on mainboard. Multiple LINE_OUT on external box.

### GPIO / Control Signals

* **Maestro GPIO**: `GPIO` [48] on Maestro has signals like `OP10_1` [62], `OP10_2` [66], `GP10 36` [71].
* **Dream GPIO**: `GPIOC8..11` [601] on SAM9707.
* **Daughter Board GPIO**: `GPIOB` [801]. `GPIOL L` [766] on daughter board connector.

### Clock Distribution

* **Audio Clocks**: `BIT_CLK` [304], `SCLK1` [297], `SCLK2` [335].
* **Dream Clocks**: `OSCI` [31], `OSCE` [38]. `SAMPLING CLOCK` [752].
* **External Box Clocks**: `SYSC` [907], `BCK` [910], `LRCIN` [912], `BEKIN` [917], `CLIKO` [926].

### Reset / Power Sequencing

* **Reset**: `ARST` [33], `CRESET` [264], `RESET` [293, 637, 763, 1097]. Control over reset lines is crucial for driver initialisation.
* **Power**: JANTA [12], +5U [11, 274], GND [43, 69, 103, 203, 249, 277, 280, 359, 377, 384, 390], -12U [4], +12U [7, 777]. JANTA might be analog power.

---

## 5. Regenerated Schematic Text

### KiCad-style Netlist Representation (Critical Paths)

```netlist
(net (code 1) (name "MAESTRO_DOUT")
  (node (ref IC_Maestro) (pin Maestro_DOUT_Pin))
  (node (ref IC_PCM1718E) (pin DIN))
)
(net (code 2) (name "LINE_OUT_L")
  (node (ref IC_PCM1718E) (pin OUTL))
  (node (ref Connector_LINE_OUT) (pin 1))
)
(net (code 3) (name "LINE_OUT_R")
  (node (ref IC_PCM1718E) (pin OUTR))
  (node (ref Connector_LINE_OUT) (pin 2))
)
(net (code 4) (name "S/PDIF_OUT")
  (node (ref IC_Maestro) (pin S/PDIF_OUT_Pin))
  (node (ref IC_CS8402) (pin SDATA))
)
(net (code 5) (name "S/PDIF_IN")
  (node (ref IC_CS8414) (pin SDATA))
  (node (ref IC_Maestro) (pin S/PDIF_IN_Pin))
)
