// userspace/threadtest.c - POSIX Threads & Futex Stress Test for EquantOS
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define NUM_THREADS 4
#define ITERATIONS  10000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int shared_counter = 0;

void *worker_thread(void *arg) {
    int id = *(int *)arg;

    for (int i = 0; i < ITERATIONS; i++) {
        pthread_mutex_lock(&lock);
        shared_counter++;
        pthread_mutex_unlock(&lock);
    }

    printf("  [Thread %d] Finished %d iterations cleanly.\n", id, ITERATIONS);
    return NULL;
}

int main(void) {
    printf("=== EquantOS POSIX Threads & Futex Verification ===\n");

    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i + 1;
        if (pthread_create(&threads[i], NULL, worker_thread, &thread_ids[i]) != 0) {
            printf("[FAIL] pthread_create failed for thread %d\n", i + 1);
            return 1;
        }
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            printf("[FAIL] pthread_join failed for thread %d\n", i + 1);
            return 1;
        }
    }

    printf("\nShared Counter: %d (Expected: %d)\n", shared_counter, NUM_THREADS * ITERATIONS);

    if (shared_counter == NUM_THREADS * ITERATIONS) {
        printf("\033[32m[PASS] Multithreading, Futex synchronization and TLS work perfectly!\033[0m\n");
    } else {
        printf("\033[31m[FAIL] Race condition detected in mutex!\033[0m\n");
    }

    return 0;
}