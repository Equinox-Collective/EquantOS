# EquantOS

```
architecture : x86_64
version      : 0.0.1-alpha
bootloader   : Limine v8
language     : C + x86_64 Assembly
libc         : musl 1.2.6
license      : GPL-2.0
```

Monolithic hobby OS written from zero. Framebuffer terminal, 
interactive shell, physical/virtual memory management, scheduler, 
VFS, FAT32/EXT2 drivers, NVMe, and Ring 3 userspace
with a musl libc port.

Boots via [Limine](https://github.com/limine-bootloader/limine) on BIOS and
UEFI. QEMU is the primary target. Has also run on real x86_64 hardware.

> **WARNING:** Early alpha. Run in a VM or on test hardware only.
> Do not store anything you care about on it.

---

## Status

Kernel version : `EquantOS 0.0.1 Alpha`

Current focus: We are entered LONG-TERM development so development is now not fully focused work but a continue of life.

### Done

- Boot: Limine base revision 3, BIOS/UEFI hybrid ISO, HHDM, framebuffer
- CPU: GDT, IDT, 8259 PIC, 100 Hz PIT, exception handling, FPU/SSE
- Memory: PMM, VMM, per-process address spaces, paging, 1 MiB kernel heap
- Processes: ELF64 loader, Ring 3 tasks, round-robin scheduler, FPU/SSE
  context save/restore, SysV initial stack, `syscall` and `int 0x80` entry
- Userspace: vendored musl 1.2.6 source, prebuilt static sysroot, `hello.elf`
  smoke test, `musltest.elf` syscall probe
- Filesystems: VFS, writable RAMFS, GPT/MBR discovery, FAT32, ext2 read/write,
  Linux-compatible `/proc` (cpuinfo, meminfo, uptime, version, loadavg, swaps,
  `/proc/<pid>/{stat,cmdline,comm,status}`, exe/cwd/fd links), `/etc/os-release`
- Ports: [fastfetch](https://github.com/fastfetch-cli/fastfetch) with a built-in EquantOS logo,
  [NetSurf](https://www.netsurf-browser.org/) 3.11 (`epacmg -S netsurf`, runs under X11/IceWM) with
  site scripts that make YouTube and GitHub usable and a built-in video player,
  [IceWM](https://ice-wm.org/) 4.1.0 with wallpapers (icewmbg) and switchable themes
- Storage: PCI enumeration, NVMe namespace I/O, legacy ATA PIO
- Input/output: PS/2 keyboard, framebuffer terminal, COM1 serial log
- X11 input: raw console keyboard mode (`KDSKBMODE`) and the console keymap
  (`KDGKBENT`), so the X server gets key codes and typing works in X programs
- Shell: interactive diagnostic shell with ~25 commands
- USB: xHCI driver, USB HID input
- Sound: Intel AC'97 playback as an OSS `/dev/dsp` (QEMU `-device AC97`)
- Installer: early-stage installer code present

### In progress

- GPU through LinuxKPI
- Own package manager (Including making network support)
- Sound implementation with PC Speaker, AC97 and continous.
- GUI

---

## Prerequisites

| Tool | Purpose |
|---|---|
| `x86_64-elf-gcc` | Main compiling |
| `nasm` | Assembly compiling |
| GNU Make | Build script |
| `xorriso` | ISO generation |
| Python 3 | Disk image generation |
| `qemu-system-x86_64` | Running the system |

### NOTE!

The repo includes musl headers and `sdk/sysroot/lib/libc.a` but the `*.o`
gitignore rule strips out the CRT startup objects. Before building userspace
you need these in `sdk/sysroot/lib/`:

```
crt1.o  crti.o  crtn.o
```

Rebuild the musl port into `sdk/sysroot/`.

---

## Commands

```sh
make             - Builds the EquantOS ISO image
make run         - Builds (if wasn't built before) and runs EquantOS inside QEMU
make debug       - Runs QEMU paused (-S) with GDB server enabled (-s)
make disks       - Generates test disk images
make clean       - Removes the build/ directory
make clean-all   - Removes build artifacts, disks, and bootloader cache
make V=1         - Enables verbose mode (prints actual GCC/LD commands)
```

> **CAUTION:** `clean-disks` and `clean-all` delete the disk images and any
> data written to them.

Creates two 64 MiB disk images if they do not exist, then launches QEMU:

| Image | Layout | Attached as |
|---|---|---|
| `disk_gpt_ext2.img` | GPT + ext2 | NVMe |
| `disk_mbr_fat32.img` | MBR + FAT32 | IDE |

Full QEMU command:

```sh
qemu-system-x86_64 \
  -m 512M -vga std -boot d \
  -cdrom build/equantos.iso \
  -device qemu-xhci,id=xhci \
  -device usb-kbd,bus=xhci.0 \
  -device usb-mouse,bus=xhci.0 \
  -drive file=disk_gpt_ext2.img,format=raw,if=none,id=nvme0 \
  -device nvme,drive=nvme0,serial=deadbeef \
  -drive file=disk_mbr_fat32.img,format=raw,if=none,id=fat0 \
  -device ide-hd,drive=fat0 \
  -device AC97 \
  -serial stdio
```

At boot, the kernel scans the NVMe device only. The GPT/ext2 partition mounts
at `/ext2`. The IDE image exists for legacy driver development and is not
currently scanned. A FAT32 partition found on the NVMe device would mount
at `/disk`.

---

## Ports

Third-party programs live in `ports/<name>/`: the vendored sources (with any
EquantOS changes applied) in `src/` and a build recipe; the resulting static
binary is committed to `res/` and packed into the ISO.

### fastfetch

`res/fastfetch.elf` (fastfetch 2.69.0) is installed as `/bin/fastfetch` in the
live system. To rebuild it (needs `cmake`, `make`, Python 3 and the `crt*.o`
objects above):

```sh
sh ports/fastfetch/build.sh
```

`ports/fastfetch/src` is upstream fastfetch 2.69.0 with two local changes:
DRM calls in `src/detection/gpu/gpu_linux.c` are guarded for systems without
`<drm/drm.h>`, and a built-in EquantOS logo is added
(`src/logo/ascii/e/equantos.txt` plus its entry in `e.inc`). The binary is
linked statically against `sdk/sysroot` using
`ports/fastfetch/equantos-toolchain.cmake`.

### NetSurf

`ports/netsurf/build.sh` builds a static NetSurf 3.11 (framebuffer frontend in
an X11 window) inside Alpine Linux and packs it as `netsurf.epkg`; the header
of the script says how. Install it with `epacmg -S netsurf` (or
`epacmg -U netsurf.epkg` for a local build) and start it from the IceWM menu
or with `netsurf-x [url]`.

On top of upstream NetSurf the port adds the following (`ports/netsurf/overlay/`
holds the new files, `ports/netsurf/patches/` the changes to existing ones):

- **Site scripts** (`content/sitejs.c`, `res/sitejs/*.js`). YouTube and GitHub
  ship applications that need a JavaScript and layout engine far beyond
  NetSurf's, but they embed the data of each page in the HTML they serve. A
  script run by an embedded QuickJS engine turns that data into markup NetSurf
  lays out well. `youtube.js` covers search, video pages, channels, playlists,
  topic feeds and comments; `github.js` covers repositories, directories, files
  with syntax colouring, issues, pull requests, commits and diffs, releases,
  search, profiles and trending. The scripts are plain files in
  `/usr/share/netsurf/sitejs/` and can be edited in place; append `?ns_raw=1`
  to an address to see the page as the site sent it.
- **Video** (`content/handlers/image/video.c`). `<video>` elements play in the
  page: ffmpeg demuxing and decoding (H.264, VP8/VP9, MPEG-4, AAC, MP3, Opus,
  Vorbis in MP4, WebM, ...), a libcurl range transfer with a sliding window,
  no threads. Sound goes to `/dev/dsp` and is the clock pictures are shown by.
  Click the picture to pause, the bar to seek. YouTube videos play from the
  single-file stream the InnerTube API returns (360p).
- A start page, web search from the address bar (DuckDuckGo's HTML front end),
  JavaScript (NetSurf's Duktape engine) switched on, and `[hidden]`,
  `<template>` and `<dialog>` content no longer rendered.

Limits: scripts of a page still run on NetSurf's ES5 engine, so sites that
are JavaScript applications and have no site script look as they did before;
live streams and anything above the single-file 360p stream are not played;
signing in to YouTube is not supported. The site scripts follow the current
page formats of both sites and need updating when those change.

---

## Known issues

- Fresh clone is missing `crt1.o`, `crti.o`, `crtn.o` - userspace will not
  link until they are provided
- The IDE/FAT32 image is not scanned by the current boot path
- The 64 MiB FAT32 image geometry is rejected by the current FAT32 driver
  as too small for FAT32
- ext2: direct-block writes only, no indirect blocks, no delete, no rename,
  no clean unmount
- ELF loader, scheduler, FD model and syscall layer are development code,
  not hardened for multi-user use