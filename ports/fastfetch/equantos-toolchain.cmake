# CMake toolchain for building Linux-ABI userspace ports against the EquantOS musl SDK.
# EquantOS implements the Linux x86_64 syscall ABI, so ports are configured as Linux targets.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-elf-gcc)
set(CMAKE_AR x86_64-elf-ar)
set(CMAKE_RANLIB x86_64-elf-ranlib)
set(CMAKE_STRIP x86_64-elf-strip)

get_filename_component(EQ_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../../sdk/sysroot" ABSOLUTE)

execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
    OUTPUT_VARIABLE EQ_GCC_INCLUDE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name
    OUTPUT_VARIABLE EQ_LIBGCC
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# musl headers first, then GCC's own (cpuid.h, stdatomic.h, intrinsics)
set(CMAKE_C_FLAGS_INIT "-nostdinc -isystem ${EQ_SYSROOT}/include -isystem ${EQ_GCC_INCLUDE} -fno-pie -fno-pic -D__linux__=1 -D__linux=1 -Dlinux=1 -D__gnu_linux__=1")

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -nostdlib -no-pie -L${EQ_SYSROOT}/lib -Wl,-z,max-page-size=0x1000 -Wl,-z,noexecstack -Wl,-Ttext-segment=0x400000 ${EQ_SYSROOT}/lib/crt1.o ${EQ_SYSROOT}/lib/crti.o")
set(CMAKE_C_STANDARD_LIBRARIES "-Wl,--start-group ${EQ_SYSROOT}/lib/libc.a ${EQ_LIBGCC} -Wl,--end-group ${EQ_SYSROOT}/lib/crtn.o")

set(CMAKE_FIND_ROOT_PATH "${EQ_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
