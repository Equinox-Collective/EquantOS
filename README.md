# EquantOS

```
architecture : x86_64
version      : 0.0.1-alpha
bootloader   : Limine v8 (BIOS + UEFI)
language     : C + x86_64 Assembly (NASM)
libc         : musl 1.2.6 (static)
license      : GPL-2.0
```

A monolithic hobby operating system written from scratch. EquantOS boots via
[Limine](https://github.com/limine-bootloader/limine), brings up its own memory
manager, scheduler, VFS, storage, USB, and network stacks, and runs unmodified
static Linux-ABI binaries in Ring 3: GNU Bash, BusyBox, and an X11 desktop
(Xfbdev with TWM or IceWM).

QEMU is the primary target. It has also booted on real x86_64 hardware.

> [!WARNING]
> Early alpha. Run it in a VM or on spare test hardware only.
> Do not keep anything you care about on disks it can write to.

---

## Table of contents

- [Features](#features)
- [Roadmap](#roadmap)
- [Repository layout](#repository-layout)
- [Building](#building)
- [Running](#running)
- [Using the system](#using-the-system)
- [Known issues](#known-issues)
- [License](#license)

---

## Features

### Core

- **Boot:** Limine base revision 3, hybrid BIOS/UEFI ISO, HHDM, framebuffer, module requests
- **CPU:** GDT, IDT, 8259 PIC, PIT at 250 Hz, exception handling and panic screen, FPU/SSE enablement
- **Memory:** buddy physical allocator, 4-level paging with 2 MiB huge pages, per-process address spaces,
  copy-on-write, slab kernel heap
- **Initcalls:** Linux-style staged init (`core` → `arch` → `subsys` → `fs` → `device` → `usb`)

### Processes and ABI

- ELF64 loader with SysV initial stack (argv, envp, auxv)
- Preemptive scheduler, Ring 3 tasks, FPU/SSE context save/restore
- `fork`, `vfork`, `clone`, `execve`, `wait4`, basic signals, `futex`, pipes
- Linux x86_64 syscall ABI over both `syscall` and `int 0x80`: about 140 syscalls,
  including file I/O, `mmap`, `poll`/`select`, sockets, and SysV shared memory
- Two EquantOS-specific syscalls: `400` (kernel diagnostics bridge) and `401` (DNS resolve)

### Filesystems and storage

- VFS with mount points, writable RAMFS root, devfs (`/dev`)
- ISO9660 (live CD, mounted at `/cdrom`), FAT32, ext2 (read/write, direct and indirect blocks)
- GPT and MBR partition discovery
- NVMe (namespace I/O via DMA), legacy ATA PIO, PCI enumeration

### Devices

- Framebuffer TTY with PSF2 fonts and ANSI colors, COM1 serial log
- PS/2 keyboard, xHCI controller with USB HID keyboard and mouse
- evdev-style input: `/dev/input/event0`, `/dev/input/event1`, `/dev/input/mice`, `/dev/fb0`
- RTC, `/dev/random` and `/dev/urandom`, reboot and power-off

### Networking

- RTL8139 NIC driver
- ARP, IPv4, ICMP, UDP, TCP (with retransmission), DHCP, DNS
- BSD sockets for userspace, AF_UNIX sockets (used by X11)
- HTTPS in userspace via BearSSL

### Userspace

- musl 1.2.6 port with a prebuilt static sysroot in `sdk/sysroot/`
- GNU Bash 5.2 + BusyBox as the default environment
- X11: Xfbdev server, `twm`, `icewm`, `xeyes`
- `epacmg`: package manager that fetches `.epkg` (tar) packages over HTTPS
- `kdiag`: calls the kernel's built-in diagnostics from Bash
- Interactive installer that copies the live system to an NVMe disk (ESP + ext2)
- Users, `su`, `passwd`, `useradd`, and a login prompt
- Built-in kernel rescue shell (about 60 diagnostic commands), used when no userland shell is found

---

## Roadmap

From [`DOCS/TODO.md`](DOCS/TODO.md). The project is in long-term development, so
these move at a hobby pace.

- [x] Package manager (`epacmg`), can be improved
- [x] Networking, including HTTPS
- [ ] GPU acceleration through LinuxKPI
- [ ] Sound: PC Speaker, then AC97 and Intel HDA
- [ ] Native GUI toolkit or a Wayland compositor alongside X11
- [ ] Hardened root/user permission model
- [ ] Installer on real hardware

---

## Repository layout

```
src/
  main.c               Kernel entry point (_start)
  linker.ld            Kernel linker script (higher-half)
  equterm/             Framebuffer terminal and built-in rescue shell
  kernel/core/         GDT/IDT/PIC, PMM, VMM, heap, initcalls, panic
  kernel/proc/         ELF loader, tasks, scheduler, syscalls, pipes, init
  kernel/fs/           VFS, RAMFS, devfs, ISO9660, FAT32, ext2, GPT/MBR
  kernel/drivers/      PCI, NVMe, ATA, USB/xHCI/HID, PS/2, TTY, serial, RTL8139
  kernel/net/          ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS, sockets
  kernel/ipc/          AF_UNIX sockets, SysV shared memory
  kernel/misc/         Installer, users, RTC, timer, power, RNG
  libs/                Freestanding string/stdio helpers for the kernel
userspace/             Native programs (epacmg, kdiag, hello, musltest, memory test)
sdk/musl/              Vendored musl 1.2.6 source
sdk/sysroot/           Prebuilt headers and static libraries (libc, libm, BearSSL, ...)
res/                   Files copied onto the ISO (fonts, .bashrc, X11/IceWM configs)
limine/                Vendored Limine binaries
create_disks.py        Generates the QEMU test disk images
OVMF.fd                UEFI firmware for QEMU
```

---

## Building

### Requirements

| Tool | Purpose |
|---|---|
| `x86_64-elf-gcc`, `x86_64-elf-ld` | Cross-compiler and linker for the kernel and userspace |
| `nasm` | Assembly sources |
| GNU Make | Build system |
| `xorriso` | ISO generation |
| Python 3 | Disk image generation |
| `qemu-system-x86_64` | Running the system |
| `curl`, `tar` | Only for refreshing Limine (`make .limine.stamp`) |

The Makefile works from a POSIX shell (Linux, macOS, MSYS2) and from plain Windows `cmd.exe`.

### Files you need to provide

Some files the build needs are not in the repository, because `.gitignore`
excludes `*.o`, `*.elf`, and `*.sh`:

| File | What it is |
|---|---|
| `sdk/sysroot/lib/crt1.o`, `crti.o`, `crtn.o` | musl C runtime startup objects. Build the musl port in `sdk/musl/` and install it into `sdk/sysroot/`. |
| `res/bash.elf`, `res/busybox.elf` | Static Bash and BusyBox built against the musl sysroot |
| `res/Xfbdev.elf`, `res/twm.elf`, `res/icewm.elf`, `res/xeyes.elf` | Static X11 server, window managers, and demo client |
| `res/start.sh`, `res/menu` | X session start script and IceWM menu |

Without these, `make` stops on the first missing prerequisite. If you only want
the kernel, run `make build/kernel.elf`.

### Commands

```sh
make              # Build build/equantos.iso
make run          # Build, create disks if missing, boot the ISO in QEMU (UEFI)
make runbd        # Boot the installed system from disk_gpt_ext2.img
make debug        # Boot paused (-S) with a GDB server on :1234 (-s)
make disks        # Create the test disk images if they do not exist
make clean        # Remove build/, net.pcap and .limine.stamp
make clean-disks  # Delete both disk images
make clean-all    # clean + clean-disks + downloaded Limine leftovers
make V=1          # Verbose: print the full compiler and linker commands
```

> [!CAUTION]
> `clean-disks` and `clean-all` delete the disk images and everything written to them,
> including an installed system.

---

## Running

`make run` boots the ISO under OVMF (UEFI) with:

- 512 MiB RAM, `std` VGA
- xHCI controller with a USB keyboard and mouse
- RTL8139 NIC on QEMU user networking (traffic is dumped to `net.pcap`)
- Serial console on stdio, which carries the kernel log

Test disks:

| Image | Size | Layout | Attached as |
|---|---|---|---|
| `disk_gpt_ext2.img` | 128 MiB | GPT: ESP (FAT32, 40 MiB) + ext2 root (64 MiB) | NVMe |
| `disk_mbr_fat32.img` | 64 MiB | MBR + FAT32 | IDE |

At boot the kernel scans only the NVMe device. Partitions are mounted at
`/drives/fat32_nvme` and `/drives/ext2_nvme`. The IDE image is kept for legacy
ATA driver development.

`make runbd` boots that NVMe disk directly, which is how you test a system
written by the installer.

---

## Using the system

On boot the kernel mounts the live CD at `/cdrom`, exposes its files under
`/bin` and `/sys/bin`, and starts the first shell it finds (`/bin/bash`, then
BusyBox). If none exists, it drops into the built-in rescue shell.

`res/.bashrc` sets up aliases, so BusyBox applets and kernel diagnostics work
as regular commands:

```sh
ls /drives          # BusyBox applet
pciscan             # -> kdiag pciscan (kernel PCI scan)
sysinfo             # -> kdiag sysinfo
installer           # Interactive installer to the NVMe disk
xstart              # Start the X11 session (Xfbdev + window manager)
icewm               # Start IceWM
```

Package manager:

```sh
epacmg sync         # or -Sy: fetch the package list
epacmg list         # or -Q:  show available packages
epacmg ins <pkg>    # or -S:  download and install a package
```

The default package list is fetched from
[`ewasion137/epacmg-trans`](https://github.com/ewasion137/epacmg-trans).

---

## Known issues

- A fresh clone does not build the ISO until the [missing files](#files-you-need-to-provide) are provided.
- The IDE/FAT32 image is not scanned at boot.
- The 64 MiB MBR FAT32 image is rejected by the FAT32 driver as too small for FAT32.
- ext2 has no delete, rename, or clean unmount yet.
- The ELF loader, scheduler, FD model, and syscall layer are development code,
  not hardened for multi-user use. User accounts are not a security boundary.
- Single CPU only (no SMP or APIC yet).

---

## License

EquantOS is licensed under the GNU General Public License v2.0. See [`LICENSE.md`](LICENSE.md).

Third-party components keep their own licenses: musl (`sdk/musl/COPYRIGHT`),
Limine, BearSSL, and the bundled binaries and fonts in `res/`.
