# Testing maestro2em.c v0.5 on real Guillemot Maxi Studio ISIS hardware

This guide is written to get you through testing safely and to produce
the specific, structured feedback that actually moves this project
forward — not just "it worked" / "it didn't."

**Read the whole "Before you start" section before doing anything.**
Nothing here can damage the card — everything is digital register I/O,
not firmware flashing or voltage manipulation — but a kernel module
poking hardware registers for the first time can hang or crash a
running system, and you should be set up to recover from that cheaply.

---

## Before you start

### Safety setup
- **Don't test on a machine you can't afford to reboot uncleanly.** A
  hang here means a hard power-cycle, not data loss on its own, but
  save/close anything else running first.
- Have physical access to the reset button, or know you can power-cycle
  it. SSH access alone isn't enough if the box hangs hard.
- Make sure secure boot is disabled or you've signed the module,
  whichever your distro requires for out-of-tree modules.

### What you need
- Linux kernel headers matching your running kernel
  (`sudo apt install linux-headers-$(uname -r)` or your distro's
  equivalent)
- Build tools: `gcc`, `make`
- `alsa-utils` (`aplay`, `arecord`, `alsamixer`) for the audio tests

### Get the code
```bash
tar -xzf isisalsa-v0.5.tar.gz
cd isisalsa
git log --oneline    # confirm you're at v0.5 (commit 32b61b2 or later)
```

---

## Stage 1 — Build

```bash
make
```

**This is itself a real test.** If it doesn't build cleanly, that's
useful feedback on its own — report the exact error, the line number,
and your kernel version (`uname -r`). Different kernel versions
occasionally rename ALSA core functions we depend on.

Expect it to build with zero errors and zero warnings. If you see
warnings, copy them exactly — even "harmless-looking" ones (unused
variable, implicit declaration) often point at a real mismatch between
what this driver assumes and what your kernel actually provides.

---

## Stage 2 — Load with diagnostics off (baseline)

This tests everything **except** the new, less-confirmed IDMA/GPIO
work — the part of the driver that's been through the most independent
cross-checking.

```bash
sudo insmod maestro2em.ko
dmesg | tail -40
```

### What to look for

**Good sign:**
```
maestro2em: DMA pool: 512 KiB @ 0x...
maestro2em: Maestro-2E initialised (clock=48828 Hz)
maestro2em: AC97 codec: 0x...
maestro2em: Guillemot Maxi Studio ISIS registered (v0.5)
```

**Report back either way:**
- The full `dmesg` output from `insmod` to the last line, verbatim —
  don't paraphrase or summarize it, copy-paste the actual text.
- Whether the machine stayed responsive throughout (if it hung, note
  *where* the last dmesg line was before it hung — that tells us
  exactly which register write to suspect).
- Output of `cat /proc/asound/cards` — does the ISIS show up at all?
- Output of `lspci -v -s $(lspci | grep 125d:1978 | cut -d' ' -f1)` —
  confirms the kernel sees the device and what resources it got
  assigned.

If it loads cleanly, this alone is a big deal — it means every
register write this session added or corrected in `maestro_chip_init()`
(the PCI config fixes, the ACPI clock-gating, the Ring Bus IDR/I2S-CHI
bits) executed against real silicon without hanging the bus.

If `insmod` itself hangs, that's precise, valuable information: it
narrows the fault to somewhere in `maestro_probe()` before the
`dev_info(...registered...)` line, and `dmesg`'s last line before the
hang tells us which step.

---

## Stage 3 — AC97 mixer

```bash
alsamixer -c ISIS    # or whatever `aplay -l` calls the card
```

- Do the expected controls show up (Master, PCM, Mic, Line, CD)?
- Do volume changes in `alsamixer` audibly affect output once you have
  something playing (Stage 4)?
- Try `amixer -c ISIS scontrols` and paste the output — this tells us
  whether `snd_ac97_mixer()` correctly enumerated the codec's real
  capabilities.

This validates the AC97 cold-reset sequence (`maestro_ac97_reset()`)
and the `DisablePR3State`/`DisableHWVolCtrl` fixes from earlier
sessions.

---

## Stage 4 — Playback (the most important single test)

```bash
speaker-test -c 2 -D hw:ISIS -t wav
# or
aplay -D hw:ISIS,0 /usr/share/sounds/alsa/Front_Center.wav
```

**Listen carefully for pitch/speed.** This is the single most
diagnostic test available to you, because of `BUG-3`/`BUG-4`: an
earlier draft of this driver used the wrong sample-rate reference
clock (48000 instead of the datasheet-correct 48828 Hz), which would
make everything play about 1.7% fast/sharp — audible to most ears if
you have a reference to compare against, and *very* audible on
sustained tones. If audio sounds correct-speed and clear (not garbled,
not pitched up), that's a strong empirical confirmation of the clock
fix. If it sounds wrong, tell us specifically: fast or slow, and by
roughly how much if you can tell.

Also report:
- Clean audio vs. crackling/dropouts/silence
- Whether both channels work (pan test: `speaker-test -c 2` cycles L/R)
- Any kernel messages during playback: `dmesg | tail -20` again after

---

## Stage 5 — On-board 2-channel capture (mic/line-in)

```bash
arecord -D hw:ISIS,0 -f S16_LE -r 48000 -c 2 -d 5 test.wav
aplay test.wav
```

Use a known input source (line-in from another device, or a mic with
something audible near it). Report:
- Does it record at all, or silence/garbage?
- Correct pitch on playback of the recording (same 1.7% check as
  Stage 4, applied to the *capture* clock path)
- Which input actually got captured — try switching sources in
  `alsamixer`'s capture panel and see if it changes what's recorded

---

## Stage 6 — The new IDMA bridge diagnostic (opt-in, informational)

**Do this only after Stages 1-5 succeed.** This exercises new,
less-confirmed code deliberately kept separate so a bad result here
can't take down the parts that already work.

```bash
sudo rmmod maestro2em
sudo insmod maestro2em.ko probe_ext_capture=1
dmesg | grep probe_ext_capture
```

Three outcomes, each independently useful:

**A — Times out after the GPIO sequence:**
```
probe_ext_capture: GPIO bits 10/11 set, GPIO_DATA now 0x....
probe_ext_capture: bit7 handshake TIMED OUT after 0x3F ...
```
Tells us the IDMA bridge itself isn't responding under these
conditions — worth trying with `probe_ext_capture=1` *and* physically
having the ISIS external breakout box connected, since the bridge may
depend on box presence.

**B — Handshake succeeds:**
```
probe_ext_capture: bit7 handshake OK, status byte = 0x..
```
This is a big result — it means the bridge, the GPIO sequence, and the
`0x3F` UART-mode command all work exactly as reconstructed. **Report
the exact status byte value.** `0xFE` would match the confirmed "SAM
ready" response from the firmware-upload path; anything else is
genuinely new information worth digging into further.

**C — System hangs on this step:**
Power-cycle, boot without `probe_ext_capture=1`, and report that it
hung specifically here. This alone tells us the GPIO 10/11 sequence or
the bridge handshake needs more scrutiny before it's safe to build on.

After this test, always:
```bash
sudo rmmod maestro2em
sudo insmod maestro2em.ko    # back to diagnostics off
```

---

## Stage 7 — Unload cleanly

```bash
sudo rmmod maestro2em
dmesg | tail -10
```

Should be silent/clean. Any warnings or the module refusing to unload
(`rmmod: ERROR: Module maestro2em is in use`) is itself worth
reporting — points at a reference-counting or resource-cleanup issue.

---

## What to send back

For maximum value per test run, please paste:

1. `uname -r` and your distro
2. Full `dmesg` output from `insmod` through `rmmod` for each stage you
   ran (concatenated is fine — timestamps let us tell them apart)
3. Explicit pass/fail on: build, load, mixer, playback (+ pitch
   correct?), capture (+ pitch correct?), and the Stage 6 diagnostic
4. Anything that felt "off" even if you can't articulate why — an odd
   pop, a control that does nothing, a volume level that's much
   quieter/louder than expected

Even a single clean run through Stages 1-5 with no diagnostic issues
is a major result for this project: it would be the first real-hardware
confirmation of the entire base driver after this many sessions of
static analysis. Stage 6 either way is new information regardless of
outcome — there's no "wasted" test here.
