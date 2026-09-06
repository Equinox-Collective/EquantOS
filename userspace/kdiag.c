// userspace/kdiag.c - EquantOS Kernel Diagnostics Bridge Tool
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define SYS_EQUANT_KDIAG 400

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: kdiag <kernel_command> [args...]\n");
        printf("Example: kdiag pciscan\n");
        printf("         kdiag mem\n");
        return 1;
    }

    char cmd_buf[256] = {0};
    size_t pos = 0;

    for (int i = 1; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (pos + len + 2 >= sizeof(cmd_buf)) break;
        
        if (i > 1) {
            cmd_buf[pos++] = ' ';
        }
        strcpy(cmd_buf + pos, argv[i]);
        pos += len;
    }

    // Direct invocation of the kernel diagnostic dispatcher
    long res = syscall(SYS_EQUANT_KDIAG, cmd_buf);
    return (int)res;
}