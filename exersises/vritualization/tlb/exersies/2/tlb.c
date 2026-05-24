#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

#define PAGE_SIZE 4096 // 4KB pages

// Read time in nanoseconds
static inline uint64_t get_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <num_pages> <num_trials>\n", argv[0]);
        return 1;
    }

    int num_pages = atoi(argv[1]);
    int num_trials = atoi(argv[2]);
    size_t array_size = (size_t)num_pages * PAGE_SIZE;

    char *array = malloc(array_size);
    if (!array) {
        perror("malloc failed");
        return 1;
    }

    // Touch each page once to ensure it's allocated and avoid first-touch penalty
    for (int i = 0; i < num_pages; i++) {
        array[i * PAGE_SIZE] = 1;
    }

    uint64_t start = get_ns();

    for (int t = 0; t < num_trials; t++) {
        for (int i = 0; i < num_pages; i++) {
            array[i * PAGE_SIZE]++;
        }
    }

    uint64_t end = get_ns();

    free(array);

    uint64_t total_ns = end - start;
    double avg_per_access = (double)total_ns / ((double)num_pages * num_trials);

    printf("Total time: %lu ns\n", total_ns);
    printf("Average time per access: %.2f ns\n", avg_per_access);

    return 0;
}
