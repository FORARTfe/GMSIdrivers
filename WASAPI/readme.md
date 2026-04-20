## Windows driver stack
---

### Architecture decisions

**WaveRT port, not WaveCyclic** — WaveCyclic (XP-era) uses kernel-managed DMA callbacks with fixed latency. WaveRT (Vista+) gives user space a direct MDL mapping of the hardware DMA buffer, which is what WASAPI exclusive mode and modern DAWs actually use. No extra OS mixing layer means latency is purely hardware-limited.

**`IMiniportWaveRTStreamNotification`** — this is the interface that makes WASAPI exclusive mode work. The driver fires a `KEVENT` on every period boundary via a `KTIMER`/`KDPC` pair. WASAPI clients (`IAudioClient` in exclusive mode) wait on this event to know when to fill/drain the next packet. ASIO4ALL bridges from ASIO to exactly this mechanism.

**28-bit DMA constraint preserved** — `MmAllocateContiguousMemorySpecifyCache` with `hi.QuadPart = (1<<28)-1` enforces the WaveCache addressing limit, same as the Linux driver.

**All hardware register values carried over** from the verified analysis:
- ES1978-specific WP `[0x07-0x0E]` from `ES197X.vxd`
- `DisableHWVolCtrl=1` → `HIRQ_HWVOL_IRQ_EN` not set anywhere
- `DisablePR3State=1` → `AC97_PR3_ANALOG_PWRDN` cleared after `Ac97HardwareInit()`
- `TurnOnOffExtAmp=1`, `GPIOVal=0x09` → GPIO amp sequence in `ChipInit()`

### ASIO path

```
DAW (ASIO host, e.g. Reaper)
    │
ASIO4ALL / FlexASIO (free, open-source ASIO wrapper)
    │ WASAPI Exclusive
IAudioClient::Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE)
    │
WaveRT port → CMaestroWaveRTStream::SetState(KSSTATE_RUN)
    │                    + RegisterNotificationEvent(PKEVENT)
    │  KTIMER fires each period → KeSetEvent
    │
ES1978 APU engine DMA → 28-bit MDL buffer → user space
```
Achievable latency at 48 kHz: **128 frames = 2.7 ms**, **256 frames = 5.3 ms**. Both are within the 10 ms threshold where humans cannot detect latency for monitoring.
