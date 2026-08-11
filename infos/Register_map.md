Based on the hardware datasheets and the low-level implementation details found in the **SAM9407.ASM** source code, the following is a comprehensive register map and command structure for the **Dream SAM9407/SAM9707** DSP.

### 1. ISA Host Interface Register Map (Hardware Level)
The host interface uses three address lines (**A[2:0]**) to map eight internal registers directly to the ISA bus.

| Offset (A2 A1 A0) | Name | Access | Function |
| :--- | :--- | :--- | :--- |
| **000 (0h)** | **Data Register** | R/W | Primary MPU-401 UART-mode data port. |
| **001 (1h)** | **Status/Control** | R/W | Status (Read) and Command (Write) for MPU-401 logic. |
| **010 (2h)** | **16-bit Burst Data** | R/W | Dedicated port for efficient 16-bit transfers (audio PCM or sound banks) using `REP OUTSW`. |
| **011 (3h)** | **Burst (High)** | - | High byte for 16-bit burst DMA mode operations. |
| **1XX (4h-7h)** | **Game Sound** | R/W | Adlib-compatible game sound emulation registers. |

***

### 2. Software Command Map (Extracted from SAM9407.ASM)
The `SAM9407.ASM` source reveals specific software-level commands used by the P16 control processor to manage synthesis and memory. These are typically sent as control bytes to initiate specific DSP routines.

| Command Code (Hex) | ASM Constant | Description |
| :--- | :--- | :--- |
| **02h** | `SAM_RD_MEM` | Initiates a memory read operation from external RAM/ROM. |
| **03h** | `SAM_GET_MMT` | Requests the **Memory Mapping Table** (MMT) from the card. |
| **48h** | `SAM_GEN_INT` | Generates a host interrupt. |
| **51h** | `SAM_GET_VOI` | Retrieves voice/channel status information. |
| **52h** | `SAM_VOI_OPEN` | Opens a new synthesis voice slot. |
| **53h** | `SAM_VOI_CLOSE` | Closes/releases a synthesis voice slot. |
| **54h** | `SAM_VOI_START` | Triggers a note or sample playback. |
| **55h** | `SAM_VOI_STOP` | Terminates playback on a specific slot. |
| **56h** | `SAM_VOI_VOL` | Sets the volume level for a voice slot. |
| **57h** | `SAM_VOI_MAIN` | Sets main synthesis parameters. |
| **58h** | `SAM_VOI_PITCH` | Controls playback frequency/pitch (Nominal: 400h for 37.5kHz). |
| **59h** | `SAM_VOI_AUX` | Manages auxiliary effects/routing. |
| **5Ah** | `SAM_VOI_FILT` | Controls the 24dB digital resonant filters. |
| **5Bh** | `SAM_VOI_MEM` | Assigns specific sample memory locations to a voice. |
| **5Ch** | `SAM_GET_POS` | Queries the current sample position/pointer. |
| **5Dh** | `SAM_ADD_POS` | Modifies the sample position for looping or offsets. |

***

### 3. Internal DSP Memory & Parameter Map
The SAM9707 architecture utilizes specialized on-chip RAM banks that the P16 processor writes to in order to control the RISC DSP synthesis engine.

| Memory Block | Configuration | Purpose |
| :--- | :--- | :--- |
| **Alg RAM** | 512 x 32 bits | Stores micro-programs (up to 32 algorithms). |
| **MA1 RAM** | 128 x 28 bits | General DSP parameter storage bank 1. |
| **MA2 RAM** | 256 x 28 bits | General DSP parameter storage bank 2. |
| **MB RAM** | 256 x 28 bits | General DSP parameter storage bank 3. |
| **MX RAM** | 256 x 16 bits | DSP data RAM (X-plane). |
| **MY RAM** | 256 x 12 bits | DSP data RAM (Y-plane). |
| **ML RAM** | 64 x 13 bits | Link RAM used to connect slots for complex synthesis. |
| **P16 Data RAM** | 256 x 16 bits | Local scratchpad for the CISC control processor. |

***

### 4. Implementation Details (Protocol)
*   **MPU-401 Status Bits:** Aside from the standard two status bits, the chip provides two additional bits to expand the MPU-401 protocol.
*   **Burst Transfers:** The port at `A[2:0] = 010` is prioritized for high-bandwidth tasks. The ASM logic shows that `InString` and sound bank uploads use this port to bypass standard MPU throttling.
*   **Sample Control:** Voice parameters like pitch are calculated using a linear scale where a value of `400h` represents a nominal frequency of 37.5kHz. Pitch updates are sent as data bytes following the `SAM_VOI_PITCH` control command.
