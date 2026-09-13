// userspace/neofetch_src.c - Native Fast System Fetch for EquantOS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

int main(void) {
    struct utsname u;
    uname(&u);

    struct sysinfo s;
    sysinfo(&s);

    long total_mb = s.totalram / (1024 * 1024);
    long free_mb  = s.freeram  / (1024 * 1024);
    long used_mb  = total_mb - free_mb;

    printf("\033[36m");
    printf("         ____                      _        \033[37mroot\033[0m@\033[36mequant\033[0m\n");
    printf("        / ___|  __ _ _   _  __ _  (_)       \033[33m-----------------\033[0m\n");
    printf("       | |___  / _` | | | |/ _` | | |       \033[32mOS\033[0m: %s x86_64\n", u.sysname);
    printf("       | |___ | (_| | |_| | (_| | | |       \033[32mKernel\033[0m: %s\n", u.release);
    printf("        \\____| \\__, |\\__,_|\\__,_| |_|       \033[32mUptime\033[0m: %ld mins\n", s.uptime / 60);
    printf("                 |_|                        \033[32mShell\033[0m: GNU Bash 5.2\n");
    printf("                                            \033[32mDisplay\033[0m: 1024x768 TrueColor (Xfbdev)\n");
    printf("                                            \033[32mMemory\033[0m: %ld MB / %ld MB\n\n", used_mb, total_mb);

    // Color Palette Blocks
    printf("       \033[40m   \033[41m   \033[42m   \033[43m   \033[44m   \033[45m   \033[46m   \033[47m   \033[0m\n");
    printf("       \033[100m   \033[101m   \033[102m   \033[103m   \033[104m   \033[105m   \033[106m   \033[107m   \033[0m\n\n");

    return 0;
}