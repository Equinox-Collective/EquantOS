/* EquantOS TCC example.
 *
 * Compile on EquantOS (LiveCD or installed system with the ISO attached):
 *
 *   tcc -static -o hello hello.c
 *   ./hello
 *
 * On the LiveCD the toolchain lives under /cdrom, so point tcc at it:
 *
 *   tcc -B /cdrom/lib/tcc -static -o hello hello.c
 *
 * Only -static output is supported (no shared libraries, no tcc -run).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void) {
    char *buf = malloc(64);
    if (!buf) {
        printf("out of memory\n");
        return 1;
    }
    strcpy(buf, "hello from EquantOS");
    printf("%s (fib(20) = %d)\n", buf, fib(20));
    free(buf);
    return 0;
}
