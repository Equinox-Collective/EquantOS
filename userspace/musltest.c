#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sched.h>

#define COLOR_GREEN "\033[0;32m"
#define COLOR_RED   "\033[0;31m"
#define COLOR_RESET "\033[0m"

void test_result(const char *test_name, long res) {
    if (res >= 0) {
        printf("[%sOK%s] %s (return value: %ld)\n", COLOR_GREEN, COLOR_RESET, test_name, res);
    } else {
        printf("[%sFAIL%s] %s (errno: %d -> %s)\n", COLOR_RED, COLOR_RESET, test_name, errno, strerror(errno));
    }
}

// 1. ТЕСТЫ БАЗОВОГО ВВОДА/ВЫВОДА (Console & Write)
void test_io(void) {
    printf("\n=== 1. Testing Standard I/O (write, read) ===\n");
    
    // Прямой вызов write через syscall
    const char *msg = "  [Syscall Raw] Hello directly from raw write syscall!\n";
    long res = syscall(SYS_write, STDOUT_FILENO, msg, strlen(msg));
    test_result("sys_write (stdout)", res);
}

// 2. ТЕСТЫ ПАМЯТИ (mmap, munmap, mprotect, brk)
void test_memory(void) {
    printf("\n=== 2. Testing Memory Management (mmap, munmap, brk) ===\n");

    // sbrk / brk
    void *current_brk = sbrk(0);
    printf("  Current brk: %p\n", current_brk);
    void *new_brk = sbrk(4096);
    test_result("sbrk(4096)", new_brk != (void*)-1 ? 0 : -1);

    // mmap (Anonymous)
    size_t page_size = 4096;
    char *ptr = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    test_result("mmap (ANONYMOUS)", ptr != MAP_FAILED ? 0 : -1);

    if (ptr != MAP_FAILED) {
        // Проверяем запись и чтение
        ptr[0] = 'A';
        ptr[4095] = 'Z';
        printf("  mmap readback: ptr[0]=%c, ptr[4095]=%c\n", ptr[0], ptr[4095]);

        // mprotect
        int prot_res = mprotect(ptr, page_size, PROT_READ);
        test_result("mprotect (PROT_READ)", prot_res);

        // munmap
        int unmap_res = munmap(ptr, page_size);
        test_result("munmap", unmap_res);
    }
}

// 3. ТЕСТЫ ИНФОРМАЦИИ О ПРОЦЕССЕ И СИСТЕМЕ
void test_process_info(void) {
    printf("\n=== 3. Testing Process & System Info ===\n");

    pid_t pid = getpid();
    test_result("getpid", pid);

    pid_t ppid = getppid();
    test_result("getppid", ppid);

    uid_t uid = getuid();
    test_result("getuid", uid);

    gid_t gid = getgid();
    test_result("getgid", gid);

    struct utsname uts;
    int uname_res = uname(&uts);
    test_result("uname", uname_res);
    if (uname_res == 0) {
        printf("  sysname:  %s\n", uts.sysname);
        printf("  nodename: %s\n", uts.nodename);
        printf("  release:  %s\n", uts.release);
        printf("  arch:     %s\n", uts.machine);
    }
}

// 4. ТЕСТЫ ВРЕМЕНИ (clock_gettime, gettimeofday, nanosleep)
void test_time(void) {
    printf("\n=== 4. Testing Time and Sleep ===\n");

    struct timeval tv;
    int gtod_res = gettimeofday(&tv, NULL);
    test_result("gettimeofday", gtod_res);
    if (gtod_res == 0) {
        printf("  sec: %ld, usec: %ld\n", tv.tv_sec, tv.tv_usec);
    }

    struct timespec ts;
    int clk_res = clock_gettime(CLOCK_MONOTONIC, &ts);
    test_result("clock_gettime (CLOCK_MONOTONIC)", clk_res);
    if (clk_res == 0) {
        printf("  sec: %ld, nsec: %ld\n", ts.tv_sec, ts.tv_nsec);
    }

    // Немного поспим (10 миллисекунд)
    struct timespec req = { .tv_sec = 0, .tv_nsec = 10000000 };
    int sleep_res = nanosleep(&req, NULL);
    test_result("nanosleep (10ms)", sleep_res);
}

// 5. ТЕСТЫ ФАЙЛОВОЙ СИСТЕМЫ (openat, read, write, close, fstat, lseek)
void test_filesystem(void) {
    printf("\n=== 5. Testing File System (openat, stat, seek) ===\n");

    // Создаем тестовый файл в /tmp (или текущей директории)
    const char *filename = "musl_test_tmp.txt";
    int fd = open(filename, O_CREAT | O_RDWR | O_TRUNC, 0644);
    test_result("open (O_CREAT | O_RDWR)", fd >= 0 ? 0 : -1);

    if (fd >= 0) {
        const char *data = "Musl OS Syscall Test String\n";
        ssize_t wbytes = write(fd, data, strlen(data));
        test_result("write to file", wbytes == (ssize_t)strlen(data) ? 0 : -1);

        // fstat
        struct stat st;
        int stat_res = fstat(fd, &st);
        test_result("fstat", stat_res);
        if (stat_res == 0) {
            printf("  file size according to stat: %ld bytes\n", st.st_size);
        }

        // lseek back to start
        off_t offset = lseek(fd, 0, SEEK_SET);
        test_result("lseek (SEEK_SET, 0)", offset == 0 ? 0 : -1);

        // read back
        char buf[64] = {0};
        ssize_t rbytes = read(fd, buf, sizeof(buf) - 1);
        test_result("read from file", rbytes > 0 ? 0 : -1);
        printf("  read content: \"%s\"\n", buf);

        close(fd);
        unlink(filename); // Удаляем за собой
    }

    // Проверка текущей рабочей директории
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("  Current CWD: %s\n", cwd);
        test_result("getcwd", 0);
    } else {
        test_result("getcwd", -1);
    }
}

// 6. ТЕСТЫ СИГНАЛОВ
void dummy_signal_handler(int sig) {
    (void)sig; // Игнорируем предупреждение
}

void test_signals(void) {
    printf("\n=== 6. Testing Signal Handling ===\n");

    // rt_sigaction
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dummy_signal_handler;
    sigemptyset(&sa.sa_mask);

    int sa_res = sigaction(SIGUSR1, &sa, NULL);
    test_result("sigaction (SIGUSR1)", sa_res);

    // raise / kill (посылаем сигнал самому себе)
    if (sa_res == 0) {
        int raise_res = raise(SIGUSR1);
        test_result("raise (SIGUSR1)", raise_res);
    }
}

// 7. ТЕСТЫ SCHEDULING И FUTEX (Планировщик и синхронизация)
void test_scheduler_and_futex(void) {
    printf("\n=== 7. Testing Scheduler & Raw Futex ===\n");

    int yield_res = sched_yield();
    test_result("sched_yield", yield_res);

    // Прямой вызов futex (FUTEX_WAKE на случайную переменную не должен падать)
    int futex_val = 0;
    long futex_res = syscall(SYS_futex, &futex_val, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
    test_result("sys_futex (FUTEX_WAKE)", futex_res);
}

int main(int argc, char *argv[], char *envp[]) {
    printf("==================================================\n");
    printf("       MUSL LIBC SYSCALL COMPREHENSIVE TEST       \n");
    printf("==================================================\n");

    printf("Passed %d arguments. Environment vars at %p\n", argc, (void*)envp);

    test_io();
    test_memory();
    test_process_info();
    test_time();
    test_filesystem();
    test_signals();
    test_scheduler_and_futex();

    printf("\n==================================================\n");
    printf("All test sections reached. Exiting via exit_group...\n");
    printf("==================================================\n");

    return 0;
}