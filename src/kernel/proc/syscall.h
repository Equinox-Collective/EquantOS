// src/kernel/proc/syscall.h - Complete Linux ABI System Call Interface
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ============================================================================
// 1. Linux Standard Error Codes (Negative Return Values)
// ============================================================================
#define EPERM            1      // Operation not permitted
#define ENOENT           2      // No such file or directory
#define ESRCH            3      // No such process
#define EINTR            4      // Interrupted system call
#define EIO              5      // I/O error
#define ENXIO            6      // No such device or address
#define E2BIG            7      // Argument list too long
#define ENOEXEC          8      // Exec format error
#define EBADF            9      // Bad file descriptor
#define ECHILD          10      // No child processes
#define EAGAIN          11      // Resource temporarily unavailable / Try again
#define ENOMEM          12      // Out of memory
#define EACCES          13      // Permission denied
#define EFAULT          14      // Bad address
#define EBUSY           16      // Device or resource busy
#define EEXIST          17      // File exists
#define EXDEV           18      // Cross-device link
#define ENODEV          19      // No such device
#define ENOTDIR         20      // Not a directory
#define EISDIR          21      // Is a directory
#define EINVAL          22      // Invalid argument
#define ENFILE          23      // File table overflow
#define EMFILE          24      // Too many open files
#define ENOTTY          25      // Not a typewriter / Inappropriate ioctl
#define ETXTBSY         26      // Text file busy
#define EFBIG           27      // File too large
#define ENOSPC          28      // No space left on device
#define ESPIPE          29      // Illegal seek
#define EROFS           30      // Read-only file system
#define EMLINK          31      // Too many links
#define EPIPE           32      // Broken pipe
#define EDOM            33      // Math argument out of domain
#define ERANGE          34      // Math result not representable
#define ENOSYS          38      // Invalid system call number
#define ENOTEMPTY       39      // Directory not empty
#define ENAMETOOLONG    36      // File name too long
#define EAFNOSUPPORT    97      // Address family not supported by protocol
#define ECONNREFUSED   111      // Connection refused

// ============================================================================
// 2. Linux x86_64 Syscall Numbers (0 - 302+)
// ============================================================================
#define SYS_READ                 0
#define SYS_WRITE                1
#define SYS_OPEN                 2
#define SYS_CLOSE                3
#define SYS_STAT                 4
#define SYS_FSTAT                5
#define SYS_LSTAT                6
#define SYS_POLL                 7
#define SYS_LSEEK                8
#define SYS_MMAP                 9
#define SYS_MPROTECT            10
#define SYS_MUNMAP              11
#define SYS_BRK                 12
#define SYS_RT_SIGACTION        13
#define SYS_RT_SIGPROCMASK      14
#define SYS_RT_SIGRETURN        15
#define SYS_IOCTL               16
#define SYS_PREAD64             17
#define SYS_PWRITE64            18
#define SYS_READV               19
#define SYS_WRITEV              20
#define SYS_ACCESS              21
#define SYS_PIPE                22
#define SYS_SELECT              23
#define SYS_SCHED_YIELD         24
#define SYS_MREMAP              25
#define SYS_MSYNC               26
#define SYS_MINCORE             27
#define SYS_MADVISE             28
#define SYS_SHMGET              29
#define SYS_SHMAT               30
#define SYS_DUP                 32
#define SYS_DUP2                33
#define SYS_PAUSE               34
#define SYS_NANOSLEEP           35
#define SYS_GETPID              39
#define SYS_SOCKET              41
#define SYS_CONNECT             42
#define SYS_ACCEPT              43
#define SYS_SENDTO              44
#define SYS_RECVFROM            45
#define SYS_SENDMSG             46
#define SYS_RECVMSG             47
#define SYS_SHUTDOWN            48
#define SYS_BIND                49
#define SYS_LISTEN              50
#define SYS_GETSOCKNAME         51
#define SYS_GETPEERNAME         52
#define SYS_SOCKETPAIR          53
#define SYS_SETSOCKOPT          54
#define SYS_GETSOCKOPT          55
#define SYS_CLONE               56
#define SYS_FORK                57
#define SYS_VFORK               58
#define SYS_EXECVE              59
#define SYS_EXIT                60
#define SYS_WAIT4               61
#define SYS_KILL                62
#define SYS_UNAME               63
#define SYS_SHMDT               67
#define SYS_FCNTL               72
#define SYS_FSYNC               74
#define SYS_FDATASYNC           75
#define SYS_TRUNCATE            76
#define SYS_FTRUNCATE           77
#define SYS_GETDENTS            78
#define SYS_GETCWD              79
#define SYS_CHDIR               80
#define SYS_FCHDIR              81
#define SYS_RENAME              82
#define SYS_MKDIR               83
#define SYS_RMDIR               84
#define SYS_CREAT               85
#define SYS_LINK                86
#define SYS_UNLINK              87
#define SYS_SYMLINK             88
#define SYS_READLINK            89
#define SYS_CHMOD               90
#define SYS_FCHMOD              91
#define SYS_CHOWN               92
#define SYS_FCHOWN              93
#define SYS_LCHOWN              94
#define SYS_UMASK               95
#define SYS_GETTIMEOFDAY        96
#define SYS_GETRLIMIT           97
#define SYS_GETRUSAGE           98
#define SYS_SYSINFO             99
#define SYS_TIMES              100
#define SYS_GETUID             102
#define SYS_GETGID             104
#define SYS_SETUID             105
#define SYS_SETGID             106
#define SYS_GETEUID            107
#define SYS_GETEGID            108
#define SYS_SETPGID            109
#define SYS_GETPPID            110
#define SYS_GETPGRP            111
#define SYS_SETSID             112
#define SYS_GETPGID            121
#define SYS_SIGALTSTACK        131
#define SYS_UTIME              132
#define SYS_ARCH_PRCTL         158
#define SYS_GETTID             186
#define SYS_TKILL              200
#define SYS_FUTEX              202
#define SYS_GETDENTS64         217
#define SYS_SET_TID_ADDRESS    218
#define SYS_CLOCK_GETTIME      228
#define SYS_CLOCK_GETRES       229
#define SYS_CLOCK_NANOSLEEP    230
#define SYS_EXIT_GROUP         231
#define SYS_TGKILL             234
#define SYS_UTIMES             235
#define SYS_OPENAT             257
#define SYS_MKDIRAT            258
#define SYS_FCHOWNAT           260
#define SYS_FUTIMESAT          261
#define SYS_NEWFSTATAT         262
#define SYS_UNLINKAT           263
#define SYS_RENAMEAT           264
#define SYS_LINKAT             265
#define SYS_SYMLINKAT          266
#define SYS_READLINKAT         267
#define SYS_FCHMODAT           268
#define SYS_FACCESSAT          269
#define SYS_PSELECT6           270
#define SYS_PPOLL              271
#define SYS_DUP3               292
#define SYS_PIPE2              293
#define SYS_PRLIMIT64          302
#define SYS_EQUANT_KDIAG       400

// ============================================================================
// 3. Flags & Constants
// ============================================================================
#define AT_FDCWD              -100
#define AT_SYMLINK_NOFOLLOW   0x100
#define AT_REMOVEDIR          0x200

#define SEEK_SET                 0
#define SEEK_CUR                 1
#define SEEK_END                 2

#define O_RDONLY                 00
#define O_WRONLY                 01
#define O_RDWR                   02
#define O_CREAT                0100
#define O_EXCL                 0200
#define O_NOCTTY               0400
#define O_TRUNC               01000
#define O_APPEND              02000
#define O_NONBLOCK            04000
#define O_DIRECTORY         0200000
#define O_CLOEXEC          02000000

#define PROT_NONE               0x0
#define PROT_READ               0x1
#define PROT_WRITE              0x2
#define PROT_EXEC               0x4

#define MAP_SHARED             0x01
#define MAP_PRIVATE            0x02
#define MAP_FIXED              0x10
#define MAP_ANONYMOUS          0x20

#define ARCH_SET_GS          0x1001
#define ARCH_SET_FS          0x1002
#define ARCH_GET_FS          0x1003
#define ARCH_GET_GS          0x1004

#define F_DUPFD                   0
#define F_GETFD                   1
#define F_SETFD                   2
#define F_GETFL                   3
#define F_SETFL                   4
#define F_DUPFD_CLOEXEC        1030

#define TCGETS               0x5401
#define TCSETS               0x5402
#define TCSETSW              0x5403
#define TCSETSF              0x5404
#define TIOCGWINSZ           0x5413
#define TIOCGPGRP            0x540F
#define TIOCSPGRP            0x5410
#define FIONREAD             0x541B

#define POLLIN               0x0001
#define POLLPRI              0x0002
#define POLLOUT              0x0004
#define POLLERR              0x0008
#define POLLHUP              0x0010
#define POLLNVAL             0x0020

#define FUTEX_WAIT                0
#define FUTEX_WAKE                1
#define FUTEX_REQUEUE             3
#define FUTEX_PRIVATE_FLAG      128
#define FUTEX_CLOCK_REALTIME    256

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

#define S_IFMT              0170000
#define S_IFSOCK            0140000
#define S_IFLNK             0120000
#define S_IFREG             0100000
#define S_IFBLK             0060000
#define S_IFDIR             0040000
#define S_IFCHR             0020000
#define S_IFIFO             0010000

#define CLOCK_REALTIME            0
#define CLOCK_MONOTONIC           1
#define CLOCK_PROCESS_CPUTIME_ID  2
#define CLOCK_THREAD_CPUTIME_ID   3
#define CLOCK_MONOTONIC_RAW       4

#define RLIMIT_CPU                0
#define RLIMIT_FSIZE              1
#define RLIMIT_DATA               2
#define RLIMIT_STACK              3
#define RLIMIT_CORE               4
#define RLIMIT_RSS                5
#define RLIMIT_NPROC              6
#define RLIMIT_NOFILE             7
#define RLIMIT_MEMLOCK            8
#define RLIMIT_AS                 9
#define RLIM_INFINITY  0xFFFFFFFFFFFFFFFFULL

#define SIG_BLOCK                 0
#define SIG_UNBLOCK               1
#define SIG_SETMASK               2

#define SIGHUP                    1
#define SIGINT                    2
#define SIGQUIT                   3
#define SIGILL                    4
#define SIGTRAP                   5
#define SIGABRT                   6
#define SIGBUS                    7
#define SIGFPE                    8
#define SIGKILL                   9
#define SIGUSR1                  10
#define SIGSEGV                  11
#define SIGUSR2                  12
#define SIGPIPE                  13
#define SIGALRM                  14
#define SIGTERM                  15
#define SIGCHLD                  17
#define SIGCONT                  18
#define SIGSTOP                  19
#define SIGTSTP                  20

// ============================================================================
// 4. Linux Kernel Structures
// ============================================================================
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) syscall_regs_t;

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct linux_timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    struct linux_timespec st_atim;
    struct linux_timespec st_mtim;
    struct linux_timespec st_ctim;
    int64_t  __unused[3];
};

struct linux_pollfd {
    int   fd;
    short events;
    short revents;
};

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

struct linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct linux_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[32];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

struct rusage {
    struct linux_timeval ru_utime;
    struct linux_timeval ru_stime;
    int64_t ru_maxrss;
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;
    int64_t ru_majflt;
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;
    int64_t ru_nivcsw;
};

struct tms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};

typedef struct {
    uint64_t total_ram;
    uint64_t free_ram;
    uint64_t used_ram;
    uint64_t pmm_total_pages;
    uint64_t pmm_used_pages;
    uint64_t kernel_heap_used;
} __attribute__((packed)) equant_sysinfo_t;

typedef struct {
    void  *ss_sp;
    int    ss_flags;
    size_t ss_size;
} stack_t;

struct linux_sigaction {
    void     (*sa_handler)(int);
    uint64_t   sa_flags;
    void     (*sa_restorer)(void);
    uint64_t   sa_mask;
};

void init_syscalls(void);
void syscall_handler(void *regs_ptr);
void linux_syscall_handler(void *regs_ptr);

#endif // SYSCALL_H