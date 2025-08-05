#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define THRESHOLD 1000  // Threshold for sloppy counter

typedef struct {
    int global_count;           // Global counter
    pthread_mutex_t global_lock; // Lock for global counter
    int num_threads;             // Number of threads
} sloppy_counter_t;

typedef struct {
    sloppy_counter_t *counter;
    int thread_id;
    int increments;
    int local_count;            // Now thread-local (no sharing)
} thread_args_t;

// Initialize the sloppy counter
void sloppy_counter_init(sloppy_counter_t *counter, int num_threads) {
    counter->global_count = 0;
    pthread_mutex_init(&counter->global_lock, NULL);
    counter->num_threads = num_threads;
}

// Update the counter (increment by 1)
void sloppy_counter_update(sloppy_counter_t *counter, int *local_count) {
    (*local_count)++;
    if (*local_count >= THRESHOLD) {
        // Transfer local to global
        pthread_mutex_lock(&counter->global_lock);
        counter->global_count += *local_count;
        pthread_mutex_unlock(&counter->global_lock);
        *local_count = 0;
    }
}

// Get the total count
int sloppy_counter_get(sloppy_counter_t *counter, int *local_counts) {
    int total = 0;
    pthread_mutex_lock(&counter->global_lock);
    total = counter->global_count;
    pthread_mutex_unlock(&counter->global_lock);
    
    // Add all local counts (now passed as array from main thread)
    for (int i = 0; i < counter->num_threads; i++) {
        total += local_counts[i];
    }
    
    return total;
}

// Clean up the counter
void sloppy_counter_destroy(sloppy_counter_t *counter) {
    pthread_mutex_destroy(&counter->global_lock);
}

// Thread function
void *thread_func(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    args->local_count = 0;  // Initialize thread-local counter
    
    for (int i = 0; i < args->increments; i++) {
        sloppy_counter_update(args->counter, &args->local_count);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <num_threads> <increments_per_thread>\n", argv[0]);
        return 1;
    }

    int num_threads = atoi(argv[1]);
    int increments_per_thread = atoi(argv[2]);

    if (num_threads <= 0 || increments_per_thread <= 0) {
        fprintf(stderr, "Both arguments must be positive integers\n");
        return 1;
    }

    sloppy_counter_t counter;
    sloppy_counter_init(&counter, num_threads);

    pthread_t *threads = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
    thread_args_t *thread_args = (thread_args_t *)malloc(num_threads * sizeof(thread_args_t));

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Create threads
    for (int i = 0; i < num_threads; i++) {
        thread_args[i].counter = &counter;
        thread_args[i].thread_id = i;
        thread_args[i].increments = increments_per_thread;
        pthread_create(&threads[i], NULL, thread_func, &thread_args[i]);
    }

    // Join threads
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    // Calculate elapsed time in milliseconds
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0;
    elapsed += (end.tv_nsec - start.tv_nsec) / 1000000.0;

    // Collect local counts for final tally
    int *local_counts = (int *)malloc(num_threads * sizeof(int));
    for (int i = 0; i < num_threads; i++) {
        local_counts[i] = thread_args[i].local_count;
    }

    // Get final count
    int final_count = sloppy_counter_get(&counter, local_counts);

    printf("Time taken: %.2f ms\n", elapsed);
    printf("Final counter value: %d\n", final_count);
    printf("Expected value: %d\n", num_threads * increments_per_thread);

    // Clean up
    sloppy_counter_destroy(&counter);
    free(threads);
    free(thread_args);
    free(local_counts);

    return 0;
}