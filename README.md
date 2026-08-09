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
development target is QEMU on x86_64, using the bundled `disk.vhd` as the test
drive.

### Implemented features

- **Boot:** Limine protocol, higher-half direct map, framebuffer discovery, and
  a hybrid BIOS/UEFI ISO
- **CPU and interrupts:** GDT, IDT, 8259 PIC, PIT timer, exception handling,
  and SSE/FPU setup
- **Memory:** physical and virtual memory managers, paging, a 1 MiB kernel
  heap, and per-process address spaces
- **Processes:** ELF64 loader, Ring 3 tasks, a preemptive round-robin scheduler,
  FPU/SSE context switching, and an `int 0x80` syscall interface
- **Filesystems:** VFS, writable in-memory RAMFS, and experimental read-only
  FAT32 with directory traversal and case-insensitive 8.3 filenames
- **Storage and devices:** ATA PIO, MBR partition discovery, PCI enumeration,
  PS/2 keyboard input, framebuffer output, and COM1 serial logging
- **Diagnostics:** panic reporting, memory and heap tests, disk inspection,
  system metrics, and an interactive shell

The bundled `equantmemtest.elf` is packaged as a Limine boot module. At boot,
the kernel copies it into RAMFS, loads it as a Ring 3 process, and uses it to
exercise user-memory allocation, syscalls, and scheduler behavior.

### Current limitations

- ATA access targets the primary master drive and uses programmed I/O.
- FAT32 is read-only, supports short 8.3 names rather than long filenames, and
  is mounted only when the first detected MBR partition has type `0x0B` or
  `0x0C`.
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

A dedicated `x86_64-elf` cross-toolchain is required; the host compiler is not
a drop-in replacement. Linux, WSL, MSYS2, and native Windows can be used as
long as the required tools are installed and discoverable.

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
qemu-system-x86_64 -cdrom build/equantos.iso -hda disk.vhd -serial stdio
```

This attaches the repository's `disk.vhd` as the primary ATA drive and sends
COM1 output to the current terminal.

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
| `writefile <name> <text>` | Create a text file in the RAMFS root |
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

If the supported FAT32 partition is present, it is mounted read-only at
`/disk`. For example:

```text
cd /disk
ls
cat README.TXT
```

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
   `/disk`.
6. Initializes the framebuffer terminal.
7. Loads `equantmemtest.elf`, enables PS/2 keyboard interrupts, and enters the
   interactive shell.

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
|   |   |-- fs/                # VFS, RAMFS, MBR, and FAT32
|   |   |-- misc/              # PIT timer and power control
|   |   `-- proc/              # Tasks, scheduler, ELF loader, and syscalls
|   |-- libs/                  # Freestanding string and stdio routines
|   `-- userland/              # Bundled Ring 3 test program
|-- DOCS/TODO.md               # Development roadmap
|-- limine.conf                # Limine boot configuration
|-- disk.vhd                   # Development disk image used by make run
`-- Makefile                   # Build, run, and cleanup targets
```

## Roadmap

The main planned areas are:

- GPT and NVMe support
- ext2/ext3/ext4 filesystems
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
