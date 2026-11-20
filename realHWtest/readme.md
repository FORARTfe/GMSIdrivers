# Retro PC Setup Checklist: Acer Veriton 7600GT Dual-Boot (Win98 SE / Linux) for GMSI & V9x Remote Agent

A complete, structured checklist to inspect, update, assemble, and configure an **Acer Veriton 7600GT** (2004)[cite: 3] with a **SATA SSD**, **Guillemot Maxi Studio ISIS** PCI audio interface, and **Windows 98 SE / Linux dual-boot** integrated with **V9x Remote Agent** for developing **GMSI drivers**.

---

## 1. Visual Inspection, Hardware Audit & Functional Testing

- [ ] **Visual Motherboard & PSU Inspection:**
  - Open the **Acer Veriton 7600GT** chassis and inspect the motherboard (Intel 865G / ICH5) and power supply (220–240V~, 50Hz, 3A)[cite: 3] for **bulging, leaking, or vented electrolytic capacitors** (common in 2004-era hardware).
  - Check the CMOS battery (**CR2032**) voltage (replace if below `2.9V` to prevent BIOS settings from resetting).
  - Clean dust from the Socket 478 CPU heatsink/fan, Northbridge/Southbridge heatsinks, and PCI/AGP/DIMM slots; reapply fresh thermal paste on the Pentium 4 CPU if needed.
- [ ] **Verify Core & Peripheral Hardware Presence/Functionality:**
  - **CPU & RAM:** Confirm Pentium 4 (Socket 478) detection and test DDR400/333/266 DIMM modules across the 4 slots using **Memtest86+** (bootable ISO/USB) before OS installation. Keep **≤ 512 MB** installed initially for the Windows 98 SE setup phase.
  - **Optical Drives (Optional for installation/testing):** Verify jumper settings (Master/Slave) and tray/laser operation on IDE drives such as the **BenQ DW1640** DVD±RW (May 2005, P/N `0F2`)[cite: 1] or **BTC BDV 316E** DVD-ROM (Oct 2004, P/N `KV.0160A.004`)[cite: 2].
  - **SATA SSD Preparation:** Connect a standard 2.5" SATA SSD (recommended capacity **≤ 120 GB / 128 GB** to avoid exceeding the 137 GB 28-bit LBA boundary during initial Win98 SE setup before patching) to one of the two onboard **SATA 1.5 Gb/s (ICH5)** ports using a standard SATA data cable and a Molex-to-SATA power adapter.
  - **Guillemot Maxi Studio ISIS:** Visually inspect the PCI card, ribbon cable, rear breakout bracket, and external 8-in/4-out rack (check the internal backup battery/caps on the rack if applicable). Install the PCI card in the slot furthest from the chipset/AGP slot to minimize EMI.

---

## 2. Firmware Updates & BIOS Configuration

- [ ] **Check & Update System BIOS (Acer Veriton 7600GT):**
  - Boot into BIOS Setup (`Del` or `F2`) and note the current BIOS revision.
  - If an updated official Acer BIOS image for the **Veriton 7600GT** is available (e.g., via Acer archives / The Retro Web), flash it safely from a clean DOS boot disk/USB drive before installing operating systems.
- [ ] **Check Optical Drive Firmware (If Installed):**
  - **BenQ DW1640:** Check current firmware (latest official is `BSRB` / `BSOB`) if using it for burning/reading test media.
  - **BTC BDV 316E:** Currently on firmware `0055`[cite: 2] (check if newer OEM revisions exist only if read issues occur).
- [ ] **Configure BIOS for SATA SSD & Dual-Boot Stability:**
  - **Onboard IDE / SATA Controller Mode:** Set to **Compatible Mode** (maps SATA channels onto legacy primary/secondary IDE ports so Windows 98 SE can natively see and boot from the SATA SSD without third-party AHCI/RAID drivers).
  - **Disable Unused Onboard Peripherals:** Disable **onboard AC'97 audio**, unused **Serial (COM)**, and **Parallel (LPT)** ports to free IRQ lines for the Guillemot Maxi Studio ISIS PCI card.
  - **PnP OS Installed:** Set to `No` (or test `Yes` if IRQ routing requires it) so BIOS assigns clean, static resources to PCI devices.
  - **Boot Order:** Set CD/DVD or USB first for installation, followed by the SATA SSD.

---

## 3. Partitioning & Dual-Boot Layout (SATA SSD)

Plan the partition table upfront (using **GParted Live** on CD/USB) so both OSes coexist cleanly on a single **MBR** (Master Boot Record) disk:

- [ ] **Initialize Disk as MBR (`msdos` partition table):** Windows 98 SE does not support GPT.
- [ ] **Create Partition Layout:**
  1. **Partition 1 (`Primary`, `FAT32`, Bootable flag, ~16–32 GB):** Windows 98 SE system and applications (`C:`).
  2. **Partition 2 (`Primary`, `FAT32`, ~32–64 GB):** Shared data/driver/build exchange partition (`D:` in Win98, mounted under `/mnt/shared` in Linux) for fast file transfers between Linux and Windows 98 SE.
  3. **Partition 3 (`Primary` or `Extended/Logical`, `ext4`, ~20–40 GB):** Linux root (`/`) partition.
  4. **Partition 4 (`Logical`, `linux-swap`, ~2 GB) + Unallocated space (~10% of SSD):** Swap space and **over-provisioning** to maintain SATA SSD wear-leveling and write performance without native Win9x TRIM.

---

## 4. Windows 98 SE Installation & Core Optimization

- [ ] **Install Windows 98 SE onto Partition 1 (`FAT32`):**
  - Boot from Windows 98 SE media (or copy the `\WIN98` folder to Partition 1 from a DOS boot disk and run `setup.exe /is /ie` for a fast SSD install).
- [ ] **Apply RAM Limit Patch (> 512 MB):**
  - Install **PATCHMEM** (Rudolph R. Loew) to patch `VMM32.VXD` / `IO.SYS`, then install full DDR RAM modules (up to 3–4 GB across the 4 DIMM slots).
- [ ] **Install Intel 865G / ICH5 Chipset Drivers:**
  - Install **Intel Chipset Software Installation Utility (INF Update) v6.3.0.1007** for Windows 98/ME and reboot.
  - Verify in *Device Manager* that DMA is enabled on the IDE/SATA channels and there are no yellow exclamation marks on PCI bridges.
- [ ] **Install Storage & USB Fixes:**
  - Apply **Enable48BitLBA / patched `ESDI_506.PDR`** if total SSD capacity approaches or exceeds 137 GB.
  - Install **NUSB 3.6** for native USB 2.0 mass storage support.
- [ ] **Install Display & DirectX Drivers:**
  - Install **Intel Extreme Graphics 2** Win9x drivers (or dedicated AGP GPU drivers / **VBEMP 9x** / **Velocity9x**).
  - Install **DirectX 9.0c** (October 2006 runtime).
- [ ] **Install Community Updates & Utilities:**
  - Install **Unofficial Windows 98 SE Service Pack 3 (U98SESP3)** (core updates only).
  - Install **7-Zip 9.20** and optionally **KernelEx**.
- [ ] **Install & Verify Guillemot Maxi Studio ISIS (v1.90):**
  - Install official **Guillemot Maxi Studio ISIS v1.90** drivers and *ISIS Console*.
  - Test audio playback, analog/digital I/O on the external rack, MIDI ports, and the Dream SAM9707 synth; document exact PCI Vendor/Device IDs, IRQs, and I/O ports.

---

## 5. Linux Installation & Dual-Boot Configuration

- [ ] **Choose a 32-bit (i386/i686) Linux Distribution with Modern Kernel/ALSA Build Tooling:**
  - Recommended options for a Socket 478 Pentium 4 with ALSA development headers: **Debian 12 (Bookworm) 32-bit (`i386`)**, **antiX Linux (32-bit)**, or **Slackware 15.0 (32-bit)**.
  - *Note:* Debian 12 is the last Debian release with full official 32-bit `i386` kernel and installer support, making it ideal for compiling and testing **GMSI ALSA drivers** (`isisalsa`) directly on real hardware alongside Windows 98 SE.
- [ ] **Install Linux on Partition 3 (`ext4`) & Configure GRUB2:**
  - Install **GRUB2** to the MBR of the SATA SSD.
  - Ensure `os-prober` detects Windows 98 SE on Partition 1 (`chainloader +1`) and adds it to the GRUB menu.
  - Configure GRUB timeout (`GRUB_TIMEOUT=5`) and optional `GRUB_DEFAULT=saved` (`GRUB_SAVEDEFAULT=true`) so remote reboots return to your active OS cleanly.
- [ ] **Configure SSD Periodic TRIM & ALSA Development Environment on Linux:**
  - Enable `fstrim.timer` (`sudo systemctl enable --now fstrim.timer`) so Linux periodically trims the `ext4` partition (and optionally manually run `fstrim` on mounted FAT32 partitions if supported by the SSD controller).
  - Install build dependencies (`build-essential`, `linux-headers-$(uname -r)`, `git`, `libasound2-dev`, `alsa-utils`, `pciutils`).
  - Run `lspci -vvv` on Linux to cross-check PCI configuration space, BARs, and subsystem IDs of the **Guillemot Maxi Studio ISIS** against the Windows 98 SE baseline.

---

## 6. V9x Remote Agent Setup & GMSI Workflow

- [ ] **Install V9x Remote Agent on Windows 98 SE:**
  - Download `V9XREMOTE.ISO` or `v9x-remote-agent--win32.zip` from [michaeldale/V9x-Remote-Agent](https://github.com/michaeldale/V9x-Remote-Agent) and run `SETUP.EXE` in Windows 98 SE.
  - Edit `C:\V9XREMOTE\AGENT.INI` (per `docs/physical-machine.md`) to set `allowed_client` to your modern development host's IP address on a **trusted isolated LAN**.
  - Enable Windows 98 SE autologon so the desktop and agent initialize automatically on boot.
- [ ] **Configure Modern Development Host:**
  - Clone [FORARTfe/GMSIdrivers](https://github.com/FORARTfe/GMSIdrivers) and [michaeldale/V9x-Remote-Agent](https://github.com/michaeldale/V9x-Remote-Agent).
  - Set up **Python 3.9+** (`mcp/v9x_mcp.py`), PowerShell (`scripts/v9xctl.ps1`), or [michaeldale/V9-Retro-Controller](https://github.com/michaeldale/V9-Retro-Controller) on your host.
  - Verify remote file transfer, command execution, screenshot/VNC capture (port `9869` / `5900`), and remote reboot between your host and the Veriton 7600GT.
- [ ] **Create a Full Disk Backup Image:**
  - Before testing custom Win9x VxD/WDM or Linux ALSA kernel modules from `GMSIdrivers`, capture a full raw/compressed backup of the SATA SSD (e.g., using **Clonezilla** or `dd` from a live USB) for instant recovery.

---

## Key Links

- **GMSI Drivers Repository:** https://github.com/FORARTfe/GMSIdrivers
- **V9x Remote Agent Repository:** https://github.com/michaeldale/V9x-Remote-Agent
- **V9 Retro Controller Repository:** https://github.com/michaeldale/V9-Retro-Controller
- **Velocity9x Repository:** https://github.com/michaeldale/velocity9x
