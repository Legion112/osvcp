#include <stdio.h>
#include <time.h>

int main(void) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    // code to measure
    clock_gettime(CLOCK_MONOTONIC_RAW, &end);

}