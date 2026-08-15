#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    printf("\n==================================================\n");
    printf("  SUCCESS! Hello World from Musl libc on EquantOS! \n");
    printf("==================================================\n\n");

    printf("[USER] Argument count (argc): %d\n", argc);

    // Проверяем malloc() из Musl libc (который вызывает наш сисколл sys_brk)
    char *buffer = (char *)malloc(128);
    if (buffer) {
        strcpy(buffer, "[USER] Heap allocation via Musl malloc() works!\n");
        printf("%s", buffer);
        free(buffer);
    } else {
        printf("[USER ERROR] malloc() failed!\n");
    }

    return 0;
}