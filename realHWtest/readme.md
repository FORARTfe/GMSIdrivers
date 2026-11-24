# Retro PC Setup Checklist: Acer Veriton 7600GT Dual-Boot (Win98 SE / Linux) for GMSI & V9x Remote Agent

A comprehensive, service-manual-accurate checklist to inspect, update, assemble, and configure an **Acer Veriton 7600GT** (2004, Main Board ID **F89M**, BIOS **`R03-B2`**, Intel **865G + ICH5** chipset, Project *Trumpet*) with a **SATA SSD**, **Guillemot Maxi Studio ISIS** PCI audio interface, and **Windows 98 SE / Linux dual-boot** integrated with **V9x Remote Agent** for developing **GMSI drivers**.

---

## 1. Visual Inspection, Hardware Audit & Functional Testing

- [ ] **Chassis Disassembly & Visual Inspection (Per Service Guide Ch. 3):**
  - Turn off the system, unplug all cables, remove the **two rear screws** holding the upper/left cover, slide the left side cover out, and gently detach the front bezel if accessing drive bays.
  - Inspect the **F89M** motherboard (Micro-ATX, Intel 865G Springdale-G Northbridge + ICH5 Southbridge + SMSC LPC47M192 Super I/O) and the FSP power supply (**200W** `FSP200-50GLV` or **250W** `FSP250-60THA`, rated 220–240V~, 50Hz, 3A) for **bulging, leaking, or vented electrolytic capacitors**.
  - Check the CMOS battery (**CR2032**, located near `JBAT1` / PCI slots): replace if voltage is below `2.9V` to prevent `CMOS Battery Failed` or `CMOS Checksum Error` messages during POST.
  - Clean dust from the Socket 478 heatsink/fan (`CPUFAN1`), rear system fan (`SYSFAN1`), Northbridge heatsink, and slots; release the two heatsink latches to inspect/reapply thermal paste on the Pentium 4 / Celeron CPU if needed.
  - Verify internal cabling on board headers: `ATX1` (20-pin), `JPW1` (ATX 12V 4-pin), `JFP1` (front panel), `JUSB2`/`JUSB3` (front USB), `JAUD1` (front audio — note: if front audio is disconnected, jumpers must be installed on pins **5–6** and **9–10** of `JAUD1` for rear Line-Out to function).
- [ ] **Verify Core & Peripheral Hardware Presence/Functionality:**
  - **CPU & RAM (Dual-Channel DDR400/333/266):** Confirm Socket 478 CPU detection (Pentium 4 up to 3.2 GHz / 800 MHz FSB or Celeron). Populate matching DDR DIMM pairs across **Channel A (`DIMM 1` / `DDR 1`, `DIMM 2` / `DDR 2`)** and **Channel B (`DIMM 3` / `DDR 3`, `DIMM 4` / `DDR 4`)** for 128-bit Dual-Channel mode (unbuffered, non-ECC, 2.5V, max 1 GB per slot / 4 GB total). Test modules with **[Memtest86+](https://www.memtest.org/)**, then use a matched pair of **Samsung 256MB DDR PC3200 CL3 (`M368L3223FTN-CCC`)** modules (512 MB total in Dual Channel, e.g., `DIMM 1` + `DIMM 3`) for the initial Windows 98 SE installation phase.
  - **Optical Drives (Optional for installation/testing):** Verify Master/Slave jumpers and tray/laser health on IDE channels `IDE1` (Primary, blue connector) or `IDE2` (Secondary) for drives such as the **BenQ DW1640** DVD±RW (May 2005, P/N `0F2`) or **BTC BDV 316E** DVD-ROM (Oct 2004, P/N `KV.0160A.004`, FW `0055`).
  - **SATA SSD Preparation:** Connect a 2.5" SATA SSD (recommended **≤ 120 GB / 128 GB** to stay below the 137 GB 28-bit LBA limit during initial Win98 SE setup) to **`SATA1` (Port 0)** on the motherboard using a 7-pin SATA data cable and a Molex-to-SATA power adapter (ICH5 supports SATA 1.5 Gb/s / 150 MB/s).
  - **Guillemot Maxi Studio ISIS:** Visually inspect the PCI card, internal ribbon cable, rear breakout bracket, and external 8-in/4-out rack. Install the ISIS PCI card into **`PCI Slot 3`** (bottom slot, furthest from the AGP slot and Northbridge/VRM) to minimize electromagnetic interference (EMI) — all 3 PCI slots are 32-bit v2.3 Master ($3.3\text{V}/5\text{V}$).

---

## 2. Firmware Status & Exact Phoenix-AwardBIOS Configuration

- [ ] **Verify Jumper Settings & System BIOS Revision:**
  - Confirm board jumpers before powering on: **`JBAT1`** on pins `1-2` (*Normal*; `2-3` clears CMOS), **`JBIOS1`** on `1-2` (*BIOS Flash Locked*; only set to `2-3` *Unlocked* if performing an explicit BIOS flash), **`JLAN1`** *Enabled* (Broadcom BCM5705 GbE).
  - Press **`DEL`** during POST to enter the **Phoenix - AwardBIOS CMOS Setup Utility** (v6.00 PG). Confirm **`Product Information`** shows `Product Name: Veriton 7600GT`, `Main Board ID: F89M`, and `System BIOS Version: R03-B2` (`Nov. 11, 2004`) — already a late official revision, so flashing is unnecessary unless a specific `F89M` update (`R03-B3`/`R03-B4`) is required (avoid Ventoy boot images that trigger `InitDiskBad` / missing `command.com`; use a direct DOS/FreeDOS boot disk or CD and verify `F89M` compatibility). Select **`Load Optimized Defaults`** if resetting CMOS.
- [ ] **Check Optical Drive Firmware (If Installed):**
  - **BenQ DW1640:** Verify firmware revision (latest official is `BSRB` / `BSOB`).
  - **BTC BDV 316E:** Factory firmware `0055` (no update needed unless disc read errors occur).
- [ ] **Configure Exact BIOS Settings (Per Service Guide Ch. 2):**
  - **`Standard CMOS Features`:** Verify Date/Time, set `Drive A` to `1.44M, 3.5 in.` (or `None` if no FDD is installed), and set `Halt On` to `All, But Keyboard` (useful for headless/remote testing).
  - **`Advanced BIOS Features`:**
    - `CPU L1 & L2 Cache`: **Enabled**
    - `Hyper-Threading Technology`: **Enabled** (Note: Windows 98 SE uses 1 logical thread; Linux 32-bit SMP kernel uses both logical threads)
    - `Quick Power On Self Test`: **Enabled**
    - `First / Second / Third Boot Device`: Set `CDROM` / `Hard Disk` as needed; configure **`Hard Disk Boot Priority`** so the SATA SSD is first.
    - `APIC Mode`: **Enabled** (expands IRQ routing beyond 15 IRQs for PCI/USB devices)
    - `Full Screen LOGO Show`: **Disabled** (displays POST messages and diagnostic checkpoints)
  - **`Advanced Chipset Features`:**
    - `DRAM Timing Selectable`: **By SPD**
    - `Init Display First`: **Onboard/AGP**
    - `On-Chip VGA`: **Enabled** (if using integrated Intel Extreme Graphics 2; note that installing an AGP 8X/4X $0.8\text{V}/1.5\text{V}$ card automatically disables the onboard VGA port).
    - `On-Chip Frame Buffer Size`: **8MB** or **16M**.
  - **`Integrated Peripherals` → `OnChip IDE Device` (CRITICAL for Win98 SE + SATA SSD):**
    - `IDE HDD Block Mode`: **Enabled**
    - `IDE DMA Transfer Access`: **Enabled**
    - `SATA Mode`: **IDE** (do *not* use `RAID`)
    - `On-Chip Serial ATA`: **Legacy Mode** (maps SATA channels onto legacy IRQs 14/15 so Windows 98 SE natively boots from and accesses the SATA SSD without third-party AHCI/RAID drivers; max 4 total ATA devices).
    - `Serial ATA Port 0/1 Mode`: Set **SATA Port 0** to **Primary Master** (and map P-ATA to Secondary Master/Slave if using an IDE optical drive).
  - **`Integrated Peripherals` → `Onboard Device` & `SuperIO Device`:**
    - `USB Controller` & `USB 2.0 Controller`: **Enabled**
    - `AC97 Audio` (Realtek ALC202A): **Disabled** (eliminates IRQ/driver conflicts with the Guillemot Maxi Studio ISIS).
    - `Onboard LAN Control` (Broadcom BCM5705): **Enabled**
    - `Onboard FDC Controller`: **Disabled** if no floppy drive is attached (frees `IRQ6`).
    - `Onboard Serial Port 1` / `Port 2` / `Parallel Port`: **Disabled** if unused (frees `IRQ4`, `IRQ3`, and `IRQ7` for PCI audio stability).
  - **`Power Management Setup`:**
    - `ACPI Function`: **Enabled**
    - `HDD Power Down` / `Suspend Mode`: **Disabled** (prevents SSD sleep timeouts or audio dropouts during remote agent sessions).
  - **`PnP/PCI Configurations`:**
    - `PNP OS Installed`: **No** (ensures BIOS assigns clean, deterministic static IRQs/resources to the ISIS PCI card before OS handoff).
    - `Resources Controlled By`: **Auto (ESCD)**
  - **`PC Health Status`:** Verify CPU/Ambient temperatures and PSU voltage rails (`+3.3V`, `+5V`, `+12V`, `VCore`) are within tolerance before installing OSes.

---

## 3. Pre-OS Hardware Verification (`lshw`) & Partitioning Layout (SATA SSD)

Boot from **[GParted Live 1.6.0-10 (`i686` / `i686-pae`)](https://sourceforge.net/projects/gparted/files/gparted-live-stable/1.6.0-10/)** (the final official release with 32-bit `i686` and `i686-pae` builds) **before installing any operating system**:

- [ ] **Audit Full Hardware Inventory with `lshw` & Save Persistent Reports:**
  - Because GParted Live runs in RAM (`tmpfs`), save output files to a persistent writable medium (such as a second USB drive or an already created/mounted partition, e.g., `/mnt/out`) rather than `/tmp` alone:
    ```bash
    # Mount a writable target directory (example: /dev/sdb1 on /mnt/out)
    mkdir -p /mnt/out
    mount /dev/sdb1 /mnt/out

    # Save full HTML hardware report (openable in any browser)
    lshw -html > /mnt/out/veriton7600gt-lshw.html

    # Save full plain-text hardware report
    lshw > /mnt/out/veriton7600gt-lshw.txt

    # Optional: compact summary & PCI/IRQ details
    lshw -short > /mnt/out/veriton7600gt-lshw-short.txt
    lspci -nnvvv > /mnt/out/veriton7600gt-lspci.txt
    ```
  - *(Note: On GParted Live you already operate with root privileges in the terminal; prefix with `sudo` if running from a non-root shell).*
  - Verify that all actual devices, chipsets, MAC addresses, PCI Vendor/Device IDs (including the **Guillemot Maxi Studio ISIS**), memory banks, and storage controllers are properly enumerated before starting OS installations.
- [ ] **Initialize Disk as MBR (`msdos`):**
  - Open **GParted** and create a single **MBR** (`msdos`) partition table on the SATA SSD (Windows 98 SE cannot boot from or read GPT disks).
- [ ] **Create Partition Layout:**
  1. **Partition 1 (`Primary`, `FAT32`, `boot` flag, ~16–32 GB):** Windows 98 SE system partition (`C:`).
  2. **Partition 2 (`Primary`, `FAT32`, ~32–64 GB):** Shared exchange volume (`D:` in Win98 SE, mounted at `/mnt/shared` in Linux) for instant binary/log/driver transfers between both operating systems.
  3. **Partition 3 (`Primary` or `Extended/Logical`, `ext4`, ~20–40 GB):** Linux root (`/`) partition.
  4. **Partition 4 (`Logical`, `linux-swap`, ~2 GB) + ~10% Unallocated Space:** Swap space plus **SSD over-provisioning** to preserve NAND wear-leveling and sustained write speed under Windows 98 SE (which lacks native TRIM).

---

## 4. Windows 98 SE Installation & Core Optimization

- [ ] **Install Windows 98 SE onto Partition 1 (`FAT32`):**
  - Boot from Windows 98 SE media (or copy `\WIN98` to Partition 1 from a DOS boot disk and run `setup.exe /is /ie` for a fast SSD installation).
- [ ] **Apply CPU & Memory Limit Fixes (> 512 MB RAM & Fast CPUs):**
  - Run **[Patch for Windows 3.11/95/98/98 SE/Me to fix CPU issues (`patcher9x`)](https://github.com/JHRobotics/patcher9x)** (open-source tool by JHRobotics — patches `VMM32.VXD` / `NDIS.VXD` / `ESDI_506.PDR` to resolve fast-CPU boot/protection errors and memory limits > 512 MB), then populate additional DDR modules (including the Hynix `PC2700` `HYMD232646C8J-J AA` modules if desired, noting memory speed will clock down to the slowest DIMM).
- [ ] **Install Intel 865G / ICH5 Chipset Drivers:**
  - Install **[Intel Chipset Software Installation Utility (INF Update) v6.3.0.1007](https://www.philscomputerlab.com/intel-chipset-drivers.html)** for Windows 98/ME and reboot.
  - Confirm in *Device Manager* that DMA is active on the primary/secondary IDE/SATA channels and no resource conflicts exist.
- [ ] **Install Network, Storage & USB Drivers:**
  - Install **[Broadcom NetXtreme BCM5705 Gigabit Ethernet Drivers](https://www.philscomputerlab.com/broadcom-network-drivers.html)** for Windows 98 SE (required for V9x Remote Agent TCP/IP connectivity).
  - Apply **[Enable48BitLBA / Patched `ESDI_506.PDR`](https://www.philscomputerlab.com/windows-98-48-bit-lba.html)** if total SSD size is ≥ 137 GB (if not already handled by `patcher9x`).
  - Install **[NUSB 3.6 (Native USB Mass Storage Drivers)](https://www.philscomputerlab.com/windows-98-usb-storage-driver.html)** for USB 2.0 flash drive support.
- [ ] **Install Display & DirectX Drivers:**
  - Install **[Intel Extreme Graphics 2 Win9x Drivers](https://www.philscomputerlab.com/intel-865g-drivers.html)** (`v14.10.3.3762` or `v13.6.1`) — or dedicated AGP GPU drivers / **[VBEMP 9x (Universal VESA Driver)](https://bearwindows.zcm.com.au/vbe9x.htm)** / **[Velocity9x](https://github.com/michaeldale/velocity9x)**.
  - Install **[DirectX 9.0c (October 2006 Redistributable)](https://www.philscomputerlab.com/directx.html)** for Windows 98 SE.
- [ ] **Install Community Updates & Utilities:**
  - Install **[Unofficial Windows 98 SE Service Pack 3 (U98SESP3)](https://www.majorgeeks.com/files/details/unofficial_windows_98_se_service_pack.html)** (select core/main updates only).
  - Install **[7-Zip 9.20](https://sourceforge.net/projects/sevenzip/files/7-Zip/9.20/)** and optionally **[KernelEx](https://sourceforge.net/projects/kernelex/)**.
- [ ] **Install & Verify Guillemot Maxi Studio ISIS (v1.90):**
  - Install official **[Guillemot Maxi Studio ISIS v1.90 Drivers](https://archive.org/details/guillemot-maxi-studio-isis-drivers)** and *ISIS Console*.
  - Verify analog/digital audio I/O on the external rack, MIDI ports, ASIO, and the Dream SAM9707 synthesizer; record exact PCI Vendor/Device IDs, IRQ assignment, and I/O port ranges.

---

## 5. Linux Installation & Dual-Boot Configuration

- [ ] **Install a 32-bit (`i386` / `i686`) Linux Distribution with Full Offline Packages & ALSA Build Headers:**
  - Recommended: **[Debian 12.15.0 (Bookworm) 32-bit (`i386`) DVD Images](https://cdimage.debian.org/cdimage/archive/12.15.0/i386/iso-dvd/)** (full offline DVD installation set — avoids network mirror issues on archived releases and is the last official Debian release with full 32-bit kernel/installer support), **[antiX Linux (32-bit)](https://antixlinux.com/)**, or **[Slackware 15.0 (32-bit)](http://www.slackware.com/)**.
  - Install onto **Partition 3 (`ext4`)** with swap on **Partition 4** and mount **Partition 2 (`FAT32`)** in `/etc/fstab` under `/mnt/shared` (`uid=1000,gid=1000,umask=002`).
- [ ] **Configure GRUB2 Dual-Boot Menu:**
  - Install **GRUB2** to the SATA SSD MBR and verify `os-prober` adds **Windows 98 SE** (`chainloader +1` on `(hd0,msdos1)`).
  - Set `GRUB_TIMEOUT=5` and `GRUB_DEFAULT=saved` (`GRUB_SAVEDEFAULT=true`) in `/etc/default/grub` (`sudo update-grub`) so remote reboots return cleanly to the currently active OS.
- [ ] **Enable Periodic SSD TRIM & ALSA Driver Build Environment:**
  - Enable `fstrim.timer` (`sudo systemctl enable --now fstrim.timer`) to maintain SSD health from Linux.
  - Install kernel/ALSA development and diagnostic packages: `build-essential`, `linux-headers-$(uname -r)`, `git`, `libasound2-dev`, `alsa-utils`, `pciutils`, `lshw`.
  - Inspect PCI hardware topology (`lspci -vvv`, `sudo lshw`, and `cat /proc/interrupts`) to cross-reference the **Guillemot Maxi Studio ISIS** BARs and IRQs with Windows 98 SE.

---

## 6. V9x Remote Agent Setup & GMSI Development Workflow

- [ ] **Install V9x Remote Agent on Windows 98 SE:**
  - Download `V9XREMOTE.ISO` or `v9x-remote-agent--win32.zip` from **[michaeldale/V9x-Remote-Agent](https://github.com/michaeldale/V9x-Remote-Agent)** and run `SETUP.EXE` on Windows 98 SE.
  - Configure `C:\V9XREMOTE\AGENT.INI` (per `docs/physical-machine.md`) and set `allowed_client` to your modern development host's IP address on a **trusted isolated LAN** (agent uses unauthenticated TCP/HTTP).
  - Enable Windows 98 SE autologon so Explorer and the agent initialize automatically on boot.
- [ ] **Configure Modern Development Host:**
  - Clone **[FORARTfe/GMSIdrivers](https://github.com/FORARTfe/GMSIdrivers)** and **[michaeldale/V9x-Remote-Agent](https://github.com/michaeldale/V9x-Remote-Agent)**.
  - Set up **[Python 3.9+](https://www.python.org/downloads/)** (`mcp/v9x_mcp.py` for MCP/LLM workflows), PowerShell (`scripts/v9xctl.ps1`), or **[michaeldale/V9-Retro-Controller](https://github.com/michaeldale/V9-Retro-Controller)**.
  - Verify network connectivity (agent port `9869` and VNC port `5900`) between your host and the Veriton 7600GT.
- [ ] **Capture a Full Baseline SSD Image:**
  - Before loading experimental Win9x VxD/WDM or Linux ALSA kernel modules from `GMSIdrivers`, image the entire SATA SSD using **[Clonezilla Live](https://clonezilla.org/downloads.php)** or `dd` from a live USB for rapid disaster recovery.

---

## Key Links

- **GParted Live 1.6.0-10 (Final 32-bit `i686` / `i686-pae` Releases):** https://sourceforge.net/projects/gparted/files/gparted-live-stable/1.6.0-10/
- **GMSI Drivers Repository:** https://github.com/FORARTfe/GMSIdrivers
- **V9x Remote Agent Repository:** https://github.com/michaeldale/V9x-Remote-Agent
- **V9 Retro Controller Repository:** https://github.com/michaeldale/V9-Retro-Controller
- **Velocity9x Repository:** https://github.com/michaeldale/velocity9x
- **Patch for Windows 3.11/95/98/98 SE/Me (`patcher9x`):** https://github.com/JHRobotics/patcher9x
- **Debian 12.15.0 Bookworm `i386` ISO-DVD Archive:** https://cdimage.debian.org/cdimage/archive/12.15.0/i386/iso-dvd/
