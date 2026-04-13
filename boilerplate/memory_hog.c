#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

int main() {
    size_t chunk = 50 * 1024 * 1024; // 50MB per step

    while (1) {
        char *ptr = malloc(chunk);
        if (!ptr) {
            perror("malloc");
            break;
        }

        // Touch memory (forces RSS)
        for (size_t i = 0; i < chunk; i += 4096) {
            ptr[i] = 1;
        }

        printf("Allocated 50MB more\n");
        usleep(100000); // small delay
    }

    sleep(10);
    return 0;
}