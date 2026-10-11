// userspace/ptytest.c - Userspace Smoke Test for EquantOS PTY Subsystem
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#define COLOR_RESET   "\033[0m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_RED     "\033[1;31m"
#define COLOR_CYAN    "\033[1;36m"
#define COLOR_YELLOW  "\033[1;33m"

static int test_count = 0;
static int pass_count = 0;

#define TEST_ASSERT(cond, msg) do { \
    test_count++; \
    if (cond) { \
        printf("  [" COLOR_GREEN "PASS" COLOR_RESET "] %s\n", msg); \
        pass_count++; \
    } else { \
        printf("  [" COLOR_RED "FAIL" COLOR_RESET "] %s (errno: %d - %s)\n", msg, errno, strerror(errno)); \
    } \
} while (0)

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("\n" COLOR_CYAN "=================================================" COLOR_RESET "\n");
    printf(COLOR_CYAN "       EquantOS PTY Subsystem Smoke Test         " COLOR_RESET "\n");
    printf(COLOR_CYAN "=================================================" COLOR_RESET "\n\n");

    // -------------------------------------------------------------------------
    // Test 1: Direct /dev/ptmx and ioctls
    // -------------------------------------------------------------------------
    printf(COLOR_YELLOW "Test 1: Opening /dev/ptmx directly..." COLOR_RESET "\n");
    int ptmx_fd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    TEST_ASSERT(ptmx_fd >= 0, "open(\"/dev/ptmx\") returned valid FD");

    if (ptmx_fd >= 0) {
        int unlock = 0;
        int res_lock = ioctl(ptmx_fd, TIOCSPTLCK, &unlock);
        TEST_ASSERT(res_lock == 0, "ioctl(ptmx_fd, TIOCSPTLCK, 0) succeeded");

        int pts_num = -1;
        int res_ptn = ioctl(ptmx_fd, TIOCGPTN, &pts_num);
        TEST_ASSERT(res_ptn == 0 && pts_num >= 0, "ioctl(ptmx_fd, TIOCGPTN) returned PTS number >= 0");
        printf("       -> PTS index allocated: %d\n", pts_num);

        char pts_path[32];
        snprintf(pts_path, sizeof(pts_path), "/dev/pts/%d", pts_num);
        int slave_direct = open(pts_path, O_RDWR | O_NOCTTY);
        TEST_ASSERT(slave_direct >= 0, "open(/dev/pts/N) returned valid FD");

        if (slave_direct >= 0) {
            close(slave_direct);
        }
        close(ptmx_fd);
    }

    // -------------------------------------------------------------------------
    // Test 2: Standard libc openpty()
    // -------------------------------------------------------------------------
    printf("\n" COLOR_YELLOW "Test 2: Standard libc openpty()..." COLOR_RESET "\n");
    int master_fd = -1;
    int slave_fd = -1;
    char pts_name[64];
    struct winsize ws_init = { .ws_row = 25, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0 };

    int res_pty = openpty(&master_fd, &slave_fd, pts_name, NULL, &ws_init);
    TEST_ASSERT(res_pty == 0, "openpty() succeeded");
    TEST_ASSERT(master_fd >= 0, "master_fd >= 0");
    TEST_ASSERT(slave_fd >= 0, "slave_fd >= 0");
    printf("       -> Master FD: %d, Slave FD: %d, Device: %s\n", master_fd, slave_fd, pts_name);

    if (res_pty != 0) {
        printf(COLOR_RED "[FATAL] openpty() failed; skipping remaining tests.\n" COLOR_RESET);
        return 1;
    }

    // -------------------------------------------------------------------------
    // Test 3: Terminal attributes and isatty()
    // -------------------------------------------------------------------------
    printf("\n" COLOR_YELLOW "Test 3: Terminal attributes & window sizing..." COLOR_RESET "\n");
    TEST_ASSERT(isatty(slave_fd) == 1, "isatty(slave_fd) returned 1");

    struct termios tio;
    int res_tc = tcgetattr(slave_fd, &tio);
    TEST_ASSERT(res_tc == 0, "tcgetattr(slave_fd) succeeded");

    // Modify a flag and set it back
    tio.c_lflag &= ~ECHO;
    res_tc = tcsetattr(slave_fd, TCSANOW, &tio);
    TEST_ASSERT(res_tc == 0, "tcsetattr(slave_fd, TCSANOW) succeeded");

    struct termios tio_readback;
    tcgetattr(slave_fd, &tio_readback);
    TEST_ASSERT((tio_readback.c_lflag & ECHO) == 0, "tcgetattr confirmed ECHO flag cleared");

    // Window size query and change
    struct winsize ws_cur;
    int res_ws = ioctl(slave_fd, TIOCGWINSZ, &ws_cur);
    TEST_ASSERT(res_ws == 0, "ioctl(slave_fd, TIOCGWINSZ) succeeded");

    struct winsize ws_new = { .ws_row = 40, .ws_col = 120, .ws_xpixel = 960, .ws_ypixel = 600 };
    res_ws = ioctl(master_fd, TIOCSWINSZ, &ws_new);
    TEST_ASSERT(res_ws == 0, "ioctl(master_fd, TIOCSWINSZ) set new size");

    ioctl(slave_fd, TIOCGWINSZ, &ws_cur);
    TEST_ASSERT(ws_cur.ws_row == 40 && ws_cur.ws_col == 120, "Slave reflected updated window dimensions (40x120)");

    // -------------------------------------------------------------------------
    // Test 4: Bidirectional I/O transfer
    // -------------------------------------------------------------------------
    printf("\n" COLOR_YELLOW "Test 4: Bidirectional I/O transfer..." COLOR_RESET "\n");
    const char *msg_to_slave = "Hello from PTY Master!\n";
    ssize_t w_bytes = write(master_fd, msg_to_slave, strlen(msg_to_slave));
    TEST_ASSERT(w_bytes == (ssize_t)strlen(msg_to_slave), "write(master_fd) sent full message");

    char recv_buf[128];
    memset(recv_buf, 0, sizeof(recv_buf));
    ssize_t r_bytes = read(slave_fd, recv_buf, sizeof(recv_buf) - 1);
    TEST_ASSERT(r_bytes > 0 && strcmp(recv_buf, msg_to_slave) == 0, "read(slave_fd) received exact message from master");

    const char *msg_to_master = "Reply from PTY Slave!\n";
    w_bytes = write(slave_fd, msg_to_master, strlen(msg_to_master));
    TEST_ASSERT(w_bytes == (ssize_t)strlen(msg_to_master), "write(slave_fd) sent full reply");

    memset(recv_buf, 0, sizeof(recv_buf));
    r_bytes = read(master_fd, recv_buf, sizeof(recv_buf) - 1);
    TEST_ASSERT(r_bytes > 0 && strstr(recv_buf, "Reply from PTY Slave") != NULL, "read(master_fd) received reply from slave");

    // -------------------------------------------------------------------------
    // Test 5: Fork with Controlling PTY & poll()
    // -------------------------------------------------------------------------
    printf("\n" COLOR_YELLOW "Test 5: Process Fork with Controlling PTY..." COLOR_RESET "\n");
    pid_t pid = fork();
    if (pid < 0) {
        TEST_ASSERT(false, "fork() failed");
    } else if (pid == 0) {
        // Child Process
        setsid();
        ioctl(slave_fd, TIOCSCTTY, 0);

        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);

        close(master_fd);
        close(slave_fd);

        int ctty = open("/dev/tty", O_RDWR);
        if (ctty >= 0) {
            write(ctty, "[CTTY_OK]", 9);
            close(ctty);
        }

        printf("EQUANTOS_PTY_CHILD_SUCCESS\n");
        fflush(stdout);
        _exit(42);
    } else {
        // Parent Process: wait for child output with pselect() and poll()
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(master_fd, &rfds);
        struct timespec tv = { .tv_sec = 2, .tv_nsec = 0 };
        int sel_res = pselect(master_fd + 1, &rfds, NULL, NULL, &tv, NULL);
        TEST_ASSERT(sel_res > 0 && FD_ISSET(master_fd, &rfds), "pselect(master_fd) woke up with read ready");

        struct pollfd pfd = { .fd = master_fd, .events = POLLIN };
        int poll_res = poll(&pfd, 1, 100);
        TEST_ASSERT(poll_res > 0 && (pfd.revents & POLLIN), "poll(master_fd) woke up with POLLIN on child output");

        memset(recv_buf, 0, sizeof(recv_buf));
        r_bytes = read(master_fd, recv_buf, sizeof(recv_buf) - 1);
        TEST_ASSERT(r_bytes > 0 && strstr(recv_buf, "[CTTY_OK]") != NULL && strstr(recv_buf, "EQUANTOS_PTY_CHILD_SUCCESS") != NULL, 
                    "Parent read expected child output via /dev/tty & stdout");

        int status = 0;
        waitpid(pid, &status, 0);
        TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 42, "Child process exited normally with code 42");
    }

    close(slave_fd);
    close(master_fd);

    printf("\n" COLOR_CYAN "=================================================" COLOR_RESET "\n");
    printf(" Results: %d/%d tests passed (%d%%)\n", pass_count, test_count, (pass_count * 100) / test_count);
    if (pass_count == test_count) {
        printf(" " COLOR_GREEN "[ALL TESTS PASSED! PTY SUBSYSTEM IS FULLY OPERATIONAL]" COLOR_RESET "\n");
    } else {
        printf(" " COLOR_RED "[SOME TESTS FAILED]" COLOR_RESET "\n");
    }
    printf(COLOR_CYAN "=================================================" COLOR_RESET "\n\n");

    return (pass_count == test_count) ? 0 : 1;
}
