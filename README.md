# EquantOS

> An experimental monolithic x86_64 operating system built from scratch for
> learning and low-level systems programming.

[![Architecture](https://img.shields.io/badge/architecture-x86__64-6f42c1)](#current-status)
[![Version](https://img.shields.io/badge/version-0.0.1--alpha-orange)](#current-status)
[![Bootloader](https://img.shields.io/badge/bootloader-Limine-2f81f7)](https://github.com/limine-bootloader/limine)
[![Language](https://img.shields.io/badge/language-C%20%2B%20Assembly-555555)](#project-layout)
[![libc](https://img.shields.io/badge/libc-musl%201.2.6-2c8ebb)](#boot-modules-and-userspace)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue)](LICENSE.md)

EquantOS is a hobby OS with a freestanding kernel, framebuffer terminal,
interactive diagnostic shell, physical and virtual memory management,
preemptive task scheduling, a VFS, experimental disk filesystems, an NVMe
driver, and early Ring 3 userspace backed by a port of musl libc.

It boots through [Limine](https://github.com/limine-bootloader/limine) on legacy
BIOS and UEFI systems. QEMU is the primary development target, although the
kernel has also been exercised on physical x86_64 hardware during development.

> [!WARNING]
> EquantOS is early alpha software. Run it in a virtual machine or on dedicated
> test hardware only. Do not use it to store important data.

## Current status

The kernel reports itself as **EquantOS 0.0.1 Alpha**. The current development
focus is expanding the Linux-like userspace ABI far enough to run statically
linked musl applications and, eventually, BusyBox or Bash.

### Implemented

- **Boot and CPU:** Limine base revision 3, BIOS/UEFI hybrid ISO, HHDM,
  framebuffer discovery, GDT, IDT, 8259 PIC, PIT, exception handling, and
  FPU/SSE setup
- **Memory:** physical and virtual memory managers, per-process address spaces,
  paging, and a 1 MiB kernel heap
- **Processes:** ELF64 `PT_LOAD` loader, Ring 3 tasks, preemptive round-robin
  scheduling, FPU/SSE context switching, a System V-style initial userspace
  stack, and both `syscall` and `int 0x80` entry paths
- **Userspace:** a vendored musl 1.2.6 source tree, a prebuilt static sysroot,
  a musl-based hello program, and a broader syscall test program
- **Filesystems:** VFS, writable RAMFS, GPT/MBR partition discovery, and
  experimental FAT32 and ext2 read/write drivers
- **Storage and devices:** PCI enumeration, NVMe namespace I/O, legacy ATA PIO
  code, PS/2 keyboard input, framebuffer output, and COM1 serial logging
- **Diagnostics:** panic reporting, heap and memory tests, PCI/disk inspection,
  system metrics, power controls, and an interactive shell

### In progress

- The Linux x86_64 syscall surface is only partially implemented. It is enough
  for the basic musl hello path, but not for a general POSIX environment.
- `userspace/musltest.c` is a coverage probe, not a passing conformance suite.
  It deliberately calls unsupported interfaces so missing kernel functionality
  is easy to identify.
- BusyBox/Bash, USB, and an installer are the next major roadmap items.

## Build prerequisites

Make sure these tools are available in `PATH`:

| Tool | Purpose |
| --- | --- |
| `x86_64-elf-gcc` and `x86_64-elf-ld` | Freestanding kernel and static userspace compilation |
| `nasm` | x86_64 assembly sources |
| GNU Make | Build orchestration |
| `xorriso` | Hybrid bootable ISO generation |
| Python 3 | Creation of the development disk images |
| Git | Fetching Limine v8 binary files when needed |
| `qemu-system-x86_64` | Running `make run` |

A dedicated `x86_64-elf` cross-toolchain is required; a normal host compiler is
not a drop-in replacement. The Makefile supports POSIX-like environments and
native Windows command syntax.

### Fresh-clone musl note

The repository contains musl headers and `sdk/sysroot/lib/libc.a`, but the
Makefile also expects these startup objects:

```text
sdk/sysroot/lib/crt1.o
sdk/sysroot/lib/crti.o
sdk/sysroot/lib/crtn.o
```

They are currently excluded by the repository-wide `*.o` ignore rule and are
therefore absent from a fresh clone. Rebuild/install the vendored musl port into
`sdk/sysroot`, or otherwise provide matching `x86_64-elf` startup objects,
before expecting the default ISO target to link `hello.elf` and `musltest.elf`.
This packaging gap is part of the current alpha state.

## Build and run

Clone the repository:

```sh
git clone https://github.com/Equinox-Collective/EquantOS.git
cd EquantOS
```

After the cross-toolchain and musl startup objects are available, build the
hybrid ISO:

```sh
make
```

The result is written to `build/equantos.iso`. If the required Limine v8 binary
files are missing from the project root, the Makefile fetches them from the
upstream `v8.x-binary` branch for ISO creation and removes the temporary root
copies afterwards.

Run in QEMU:

```sh
make run
```

The run target creates two 64 MiB development images if they do not already
exist, preserving existing images on later runs:

| Image | Layout | QEMU attachment |
| --- | --- | --- |
| `disk_gpt_ext2.img` | GPT with one minimal ext2 partition | NVMe device |
| `disk_mbr_fat32.img` | MBR with one FAT-style partition | IDE disk |

The effective QEMU configuration is:

```sh
qemu-system-x86_64 -boot d \
  -cdrom build/equantos.iso \
  -drive file=disk_gpt_ext2.img,format=raw,if=none,id=nvme0 \
  -device nvme,drive=nvme0,serial=deadbeef \
  -drive file=disk_mbr_fat32.img,format=raw,if=none,id=fat0 \
  -device ide-hd,drive=fat0 \
  -serial stdio
```

At present, the kernel entry point scans the NVMe device only. With the default
QEMU layout, the GPT/ext2 partition is expected at `/ext2`. The IDE image is
created and attached for legacy-driver development, but is not mounted by the
current boot path. A compatible FAT32 partition found on the scanned NVMe
device would be mounted at `/disk`.

### Make targets

| Target | Action |
| --- | --- |
| `make` | Build `build/equantos.iso` |
| `make disks` | Create missing 64 MiB development disk images |
| `make run` | Build, create disks, and start QEMU |
| `make clean` | Remove `build/` |
| `make clean-disks` | Remove both generated disk images |
| `make clean-all` | Remove build output, disk images, and temporary Limine root files |

> [!CAUTION]
> `make clean-disks` and `make clean-all` delete the development images and all
> data stored in them.

## Boot modules and userspace

The ISO currently imports these Limine modules into the root RAMFS:

| Path after boot | Source | Purpose |
| --- | --- | --- |
| `/font.psf` | `res/font.psf` | PSF font asset |
| `/hello.elf` | `userspace/hello.c` | musl `printf`, `malloc`, string, and exit smoke test |
| `/musltest.elf` | `userspace/musltest.c` | broad syscall compatibility probe |

Run an imported executable from the shell:

```text
run /hello.elf
run /musltest.elf
```

There is also a standalone `build/iso/equantmemtest.elf` Makefile rule for the
older `int 0x80` memory/scheduler stress program. It is not currently a
dependency of the ISO and is not listed in `limine.conf`, so the default build
does not import it into RAMFS.

## Linux-like syscall ABI

The current dispatcher uses Linux x86_64 syscall numbers and supports native
`syscall`; the earlier `int 0x80` gate remains available for test code. The
implemented dispatcher cases are:

| Number | Name | Current behavior |
| ---: | --- | --- |
| 0 | `read` | Read from an open VFS descriptor |
| 1 | `write` | Write to stdout/stderr or an open VFS descriptor |
| 2 | `open` | Resolve and open an existing VFS path |
| 3 | `close` | Close a process file descriptor |
| 5 | `fstat` | Placeholder that currently returns success without filling metadata |
| 9 | `mmap` | Allocate anonymous writable user pages; most arguments are ignored |
| 11 | `munmap` | Placeholder that currently returns success without unmapping pages |
| 12 | `brk` | Query, grow, or shrink the process break |
| 14 | `rt_sigprocmask` | No-op compatibility placeholder |
| 16 | `ioctl` | Currently unsupported and returns failure |
| 20 | `writev` | Sequential writes over an iovec array |
| 39 | `getpid` | Return the current task ID |
| 60 | `exit` | Mark the current task as a zombie and yield |
| 79 | `getcwd` | Return the process working directory |
| 80 | `chdir` | Change the process working directory |
| 158 | `arch_prctl` | Set or get the task FS base |
| 217 | `getdents64` | Enumerate a VFS directory |
| 218 | `set_tid_address` | Compatibility response using the current task ID |
| 231 | `exit_group` | Same current behavior as `exit` |
| 257 | `openat` | Resolve and open an existing VFS path; flags are not yet honored |

This ABI is experimental and is not fully Linux- or POSIX-compatible. In
particular, file creation flags, complete stat data, signals, clocks, futexes,
process management, networking, and many other services are still missing.

## Shell commands

Enter `help` at the `EquantOS>` prompt to print the command list.

| Command | Description |
| --- | --- |
| `help` | List shell commands |
| `clear` | Clear the framebuffer terminal |
| `echo <text>` | Print text |
| `uptime` | Show elapsed time since boot |
| `eqfetch` | Display the EquantOS banner |
| `ver` | Show the kernel version and build target |
| `ls` | List the current VFS directory |
| `pwd` | Print the current directory |
| `cd [path]` | Change directory; no argument returns to `/` |
| `cat <file>` | Print a file |
| `hexdump <file>` | Print file contents in hexadecimal |
| `writefile <path> [text]` | Create or overwrite a text file through VFS |
| `cp <source> <destination>` | Copy a regular file through VFS |
| `run <elf>` | Load an ELF64 file as a Ring 3 process |
| `mem` | Show RAM and kernel-heap usage |
| `memstress` | Run a kernel-heap allocation stress test |
| `heapdump` | Write the heap block map to the serial log |
| `sysinfo` | Show kernel memory metrics |
| `diskinfo` | Show data recorded by the legacy MBR path |
| `pciscan` | Rescan PCI devices |
| `reboot` | Reboot the machine |
| `shutdown` | Power off the machine |
| `panic_test` | Deliberately trigger an invalid-opcode kernel panic |

Example session with the default NVMe/ext2 image:

```text
ls
cd /ext2
ls
writefile notes.txt hello from EquantOS
cat notes.txt
cd /
run hello.elf
```

Filesystem write paths are experimental. Back up disk images that contain data
you care about.

> [!CAUTION]
> `panic_test` intentionally crashes the kernel and exists only to exercise the
> panic path.

## Boot sequence

At a high level, the current kernel entry point:

1. Initializes COM1, FPU/SSE, the GDT, IDT, and a 100 Hz PIT.
2. Reads the Limine HHDM response and initializes the framebuffer terminal.
3. Initializes the PMM, VMM, and 1 MiB kernel heap.
4. Starts tasking, the scheduler, and the syscall dispatcher.
5. Mounts RAMFS at `/` and imports all Limine modules.
6. Scans PCI, initializes NVMe, and discovers GPT partitions with MBR fallback.
7. Tries FAT32 first and ext2 second for each detected NVMe partition, exposing
   successful mounts at `/disk` or `/ext2`.
8. Enables PS/2 keyboard input and enters the interactive shell.

## Project layout

```text
.
|-- src/
|   |-- main.c                 # Kernel entry point and initialization
|   |-- linker.ld              # Kernel linker script
|   |-- equterm/               # Framebuffer terminal and shell
|   |-- kernel/
|   |   |-- core/              # CPU setup, interrupts, panic, PMM/VMM/heap
|   |   |-- drivers/           # NVMe, ATA, PCI, keyboard, serial, display
|   |   |-- fs/                # VFS, RAMFS, GPT/MBR, FAT32, ext2
|   |   |-- misc/              # PIT timer and power control
|   |   `-- proc/              # Tasks, scheduler, ELF loader, syscalls
|   `-- libs/                  # Freestanding string and stdio routines
|-- userspace/                 # hello, musl syscall probe, memory stress test
|-- sdk/
|   |-- musl/                  # Vendored musl 1.2.6 source tree
|   `-- sysroot/               # Installed headers and static libraries
|-- res/font.psf               # Boot font module
|-- DOCS/TODO.md               # Working roadmap
|-- create_disks.py            # MBR/FAT-style and GPT/ext2 image generator
|-- limine.conf                # Kernel and boot-module configuration
`-- Makefile                   # Build, run, disk, and cleanup targets
```

## Known limitations

- A fresh clone needs the missing musl CRT startup objects before the default
  userspace link can complete.
- The default QEMU IDE/FAT image is not scanned by the current kernel entry
  point. The active boot-time storage path is NVMe.
- The generated 64 MiB FAT-style image uses a geometry that the current FAT32
  driver rejects as too small for FAT32, independently of the IDE scan gap.
- FAT32 supports short case-insensitive 8.3 names only. Long filenames,
  deletion, and renaming are not implemented.
- ext2 write support is limited to regular files using direct inode blocks;
  indirect-block writes, deletion, renaming, and clean unmount are missing.
- The ELF loader, scheduler, file descriptor model, and syscall layer are
  development implementations, not hardened multi-user facilities.
- There is no general-purpose shell/userspace, networking, USB stack, audio,
  graphics stack, package manager, or installer yet.

## Roadmap

The working order in `DOCS/TODO.md` is:

1. BusyBox or Bash
2. USB
3. Installer

Longer-term ideas include LinuxKPI-based GPU support, a package manager, sound,
and a GUI or native graphical DSL.

## Contributing

Focused bug reports, documentation fixes, and small reviewable changes are
welcome. For kernel changes, describe the QEMU or hardware configuration used
for testing and include relevant COM1 serial output when possible.

## License

EquantOS is distributed under the [GNU General Public License v2.0](LICENSE.md).
