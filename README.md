# EquantOS

> An experimental monolithic x86_64 operating system built from scratch for
> learning and low-level systems programming.

[![Architecture](https://img.shields.io/badge/architecture-x86__64-6f42c1)](#implemented-features)
[![Version](https://img.shields.io/badge/version-0.0.1--alpha-orange)](#project-status)
[![Bootloader](https://img.shields.io/badge/bootloader-Limine-2f81f7)](https://github.com/limine-bootloader/limine)
[![Language](https://img.shields.io/badge/language-C%20%2B%20Assembly-555555)](#project-layout)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue)](LICENSE.md)

EquantOS is a hobby OS with a freestanding kernel, memory management,
framebuffer terminal, diagnostic shell, filesystem layer, hardware drivers,
preemptive task scheduler, and early Ring 3 userspace support. It boots through
[Limine](https://github.com/limine-bootloader/limine) on legacy BIOS and UEFI
systems.

> [!WARNING]
> EquantOS is early alpha software. Run it in a virtual machine or on dedicated
> development hardware only; do not use it to store important data.

## Project status

The kernel currently identifies itself as **EquantOS 0.0.1 Alpha**. The main
development target is QEMU on x86_64. The default VM uses the bundled
`disk.vhd` as its primary ATA drive and a generated raw ext2 image as its
secondary ATA drive.

### Implemented features

- **Boot:** Limine protocol, higher-half direct map, framebuffer discovery, and
  a hybrid BIOS/UEFI ISO
- **CPU and interrupts:** GDT, IDT, 8259 PIC, PIT timer, exception handling,
  and SSE/FPU setup
- **Memory:** physical and virtual memory managers, paging, a 1 MiB kernel
  heap, and per-process address spaces
- **Processes:** ELF64 loader, Ring 3 tasks, a preemptive round-robin scheduler,
  FPU/SSE context switching, and an `int 0x80` syscall interface
- **Filesystems:** VFS, writable in-memory RAMFS, and experimental FAT32 and
  ext2 drivers with file reading, creation, and writing
- **Storage and devices:** ATA PIO reads and writes on the primary
  master/slave drives, MBR partition discovery, PCI enumeration, PS/2 keyboard
  input, framebuffer output, and COM1 serial logging
- **Diagnostics:** panic reporting, memory and heap tests, disk inspection,
  system metrics, and an interactive shell

The build also produces an `equantmemtest.elf` Ring 3 test binary and places it
in the ISO staging tree. The current `limine.conf` imports only `font.psf` as a
boot module, so the test program is not copied to RAMFS or started
automatically.

### Current limitations

- ATA access uses 28-bit programmed I/O on the primary channel. The current
  setup uses its master and slave devices; there is no AHCI or NVMe support.
- FAT32 is mounted at `/disk` only when the first MBR partition on the primary
  master has type `0x0B` or `0x0C`. It supports short, case-insensitive 8.3
  names only; names created by the kernel are uppercased and truncated to that
  format.
- The raw ext2 filesystem on the primary slave is mounted at `/ext2`. Its
  write path currently handles regular files through the 12 direct inode
  blocks only; indirect-block writes, deletion, renaming, and clean unmounting
  are not implemented.
- The ELF loader and syscall ABI are minimal; there is no general-purpose
  userspace, libc, POSIX layer, networking, USB stack, or installer yet.
- The shell and drivers are intended for development and diagnostics, not
  production workloads.

## Requirements

Make sure these tools are available in `PATH`:

| Tool | Purpose |
| --- | --- |
| `x86_64-elf-gcc` and `x86_64-elf-ld` | Freestanding cross-compilation and linking |
| `nasm` | Building the x86_64 assembly sources |
| GNU Make | Build orchestration |
| `xorriso` | Hybrid bootable ISO generation |
| Git | Downloading Limine binaries when they are missing |
| `qemu-system-x86_64` | Running EquantOS with `make run` |
| `mkfs.ext2` or `winmakeext2` | Creating the ext2 test disk for `make run` |

A dedicated `x86_64-elf` cross-toolchain is required; the host compiler is not
a drop-in replacement. On POSIX environments the Makefile uses `dd` and
`mkfs.ext2`; on native Windows it expects `winmakeext2`. Linux, WSL, MSYS2, and
native Windows can be used as long as the matching tools are installed and
discoverable.

## Build and run

Clone the repository and build the ISO:

```sh
git clone https://github.com/Equinox-Collective/EquantOS.git
cd EquantOS
make
```

The resulting image is written to `build/equantos.iso`. When the Limine
binaries are not present, the Makefile downloads the `v8.x-binary` branch from
the upstream Limine repository, copies the required files, and removes the
temporary clone.

Run EquantOS in QEMU:

```sh
make run
```

The `run` target is equivalent to:

```sh
qemu-system-x86_64 -cdrom build/equantos.iso -hda disk.vhd -hdb disk.img -serial stdio
```

`make run` creates a 32 MiB raw `disk.img` when it does not exist, formats it
as ext2, and then starts QEMU. The repository's `disk.vhd` is attached as the
primary master and exposed through FAT32 at `/disk`; `disk.img` is attached as
the primary slave and mounted at `/ext2`. COM1 output is sent to the current
terminal.

Available maintenance targets:

```sh
make clean      # Remove build outputs
make clean-all  # Also remove downloaded Limine binaries from the project root
```

## Shell commands

After boot, enter `help` at the `EquantOS>` prompt to print the command list.

| Command | Description |
| --- | --- |
| `help` | List all shell commands |
| `clear` | Clear the framebuffer terminal |
| `echo <text>` | Print text |
| `uptime` | Show time elapsed since boot |
| `eqfetch` | Display the EquantOS system banner |
| `ver` | Show the kernel version and build target |
| `ls` | List the current VFS directory |
| `pwd` | Print the current directory |
| `cd [path]` | Change directory; without a path, return to `/` |
| `cat <file>` | Print a file, resolving relative paths from the current directory |
| `hexdump <file>` | Print file contents in hexadecimal |
| `writefile <path> [text]` | Create or write a file through VFS |
| `cp <source> <destination>` | Copy a regular file between mounted filesystems |
| `run <elf>` | Load an ELF64 executable from VFS as a new process |
| `mem` | Show physical-memory and kernel-heap usage |
| `memstress` | Run the kernel-heap allocation stress test |
| `heapdump` | Write the heap block map to the serial log |
| `sysinfo` | Show detailed memory metrics |
| `diskinfo` | Show ATA drive and MBR partition information |
| `pciscan` | Rescan PCI devices and write the results to the serial log |
| `reboot` | Reboot the machine |
| `shutdown` | Power off the machine |
| `panic_test` | Deliberately trigger an invalid-opcode kernel panic |

The default QEMU setup exposes writable FAT32 and ext2 mounts, so files can be
inspected and copied between them:

```text
cd /disk
ls
cat README.TXT
writefile NOTES.TXT hello from EquantOS
cp NOTES.TXT /ext2/notes.txt
cd /ext2
cat notes.txt
```

These write paths are experimental. Keep a backup of any disk image that
contains data you care about.

> [!CAUTION]
> `panic_test` intentionally crashes the kernel and exists only for testing the
> panic path.

## Syscall interface

Ring 3 programs invoke the early EquantOS ABI with `int 0x80`. The currently
handled syscall numbers are:

| Number | Name | Purpose |
| ---: | --- | --- |
| 0 | `read` | Read from an open VFS file descriptor |
| 1 | `write` | Write to a file descriptor or to stdout/stderr |
| 2 | `open` | Open a VFS path |
| 3 | `close` | Close a VFS file descriptor |
| 12 | `brk` | Query or grow the process heap |
| 39 | `getpid` | Return the current process ID |
| 60 | `exit` | Terminate the current task |
| 99 | `sysinfo` | Read EquantOS memory metrics |
| 158 | `yield` | Yield to the scheduler |

This interface is still evolving and should not be treated as stable or fully
Linux-compatible.

## Boot sequence

At a high level, the kernel:

1. Initializes COM1, SSE, the GDT, IDT, PIC, and PIT.
2. Reads the Limine HHDM and memory map, then initializes the PMM, VMM, and
   kernel heap.
3. Starts tasking, the scheduler, and the syscall dispatcher.
4. Mounts RAMFS at `/` and imports Limine boot modules into it.
5. Probes PCI and ATA, scans MBR partitions, and attempts to mount FAT32 at
   `/disk` from the primary master.
6. Attempts to mount the raw ext2 image from the primary slave at `/ext2`.
7. Initializes the framebuffer terminal and loads `/font.psf` from RAMFS.
8. Enables PS/2 keyboard interrupts and enters the interactive shell.

## Project layout

```text
.
|-- src/
|   |-- main.c                 # Kernel entry point and initialization
|   |-- linker.ld              # Kernel linker script
|   |-- equterm/               # Framebuffer terminal and diagnostic shell
|   |-- kernel/
|   |   |-- core/              # CPU setup, interrupts, panic, PMM/VMM/heap
|   |   |-- drivers/           # ATA, keyboard, PCI, serial, and display assets
|   |   |-- fs/                # VFS, RAMFS, MBR, FAT32, and ext2
|   |   |-- misc/              # PIT timer and power control
|   |   `-- proc/              # Tasks, scheduler, ELF loader, and syscalls
|   |-- libs/                  # Freestanding string and stdio routines
|   `-- userland/              # Bundled Ring 3 test program
|-- DOCS/TODO.md               # Development roadmap
|-- limine.conf                # Limine boot configuration
|-- disk.vhd                   # FAT32 development disk used by make run
|-- disk.img                   # Generated ext2 test disk (ignored by Git)
`-- Makefile                   # Build, run, and cleanup targets
```

## Roadmap

The main planned areas are:

- GPT and NVMe support
- a more complete and robust ext2 implementation, followed by ext3/ext4
- a broader Ring 3 userspace and SDK
- a POSIX-compatible libc/syscall layer, BusyBox, and a full shell
- USB support and an installer
- longer term: package management, graphics drivers, and a GUI

See [`DOCS/TODO.md`](DOCS/TODO.md) for the working checklist.

## Contributing

Focused bug reports, documentation improvements, and small, reviewable pull
requests are welcome. For kernel changes, describe how you tested the change in
QEMU and include relevant serial output when possible.

## License

EquantOS is distributed under the [GNU General Public License v2.0](LICENSE.md).
