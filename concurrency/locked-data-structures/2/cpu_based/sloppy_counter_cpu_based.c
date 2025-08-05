#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

#define THRESHOLD 1000  // Threshold for sloppy counter

typedef struct {
    int global_count;           // Global counter
    pthread_mutex_t global_lock; // Lock for global counter
    
    int *local_counts;           // One counter per CPU core
    pthread_mutex_t *local_locks; // Locks for local counters
    int num_cpus;               // Number of CPU cores
} sloppy_counter_t;

typedef struct {
    sloppy_counter_t *counter;
    int thread_id;
    int increments;
    int cpu_core;               // Which CPU core this thread is mapped to
} thread_args_t;

// Get number of available CPU cores
int get_num_cpus() {
    return sysconf(_SC_NPROCESSORS_ONLN);
}

// Initialize the sloppy counter
void sloppy_counter_init(sloppy_counter_t *counter) {
    counter->num_cpus = get_num_cpus();
    printf("System has %d CPU cores\n", counter->num_cpus);
    
    counter->global_count = 0;
    pthread_mutex_init(&counter->global_lock, NULL);
    
    counter->local_counts = (int *)calloc(counter->num_cpus, sizeof(int));
    counter->local_locks = (pthread_mutex_t *)malloc(counter->num_cpus * sizeof(pthread_mutex_t));
    
    for (int i = 0; i < counter->num_cpus; i++) {
        pthread_mutex_init(&counter->local_locks[i], NULL);
    }
}

// Update the counter (increment by 1)
void sloppy_counter_update(sloppy_counter_t *counter, int cpu_core) {
    pthread_mutex_lock(&counter->local_locks[cpu_core]);
    counter->local_counts[cpu_core]++;
    if (counter->local_counts[cpu_core] >= THRESHOLD) {
        // Transfer local to global
        pthread_mutex_lock(&counter->global_lock);
        counter->global_count += counter->local_counts[cpu_core];
        pthread_mutex_unlock(&counter->global_lock);
        counter->local_counts[cpu_core] = 0;
    }
    pthread_mutex_unlock(&counter->local_locks[cpu_core]);
}

// Get the total count
int sloppy_counter_get(sloppy_counter_t *counter) {
    int total = 0;
    pthread_mutex_lock(&counter->global_lock);
    total = counter->global_count;
    pthread_mutex_unlock(&counter->global_lock);
    
    // Add all local counts
    for (int i = 0; i < counter->num_cpus; i++) {
        pthread_mutex_lock(&counter->local_locks[i]);
        total += counter->local_counts[i];
        pthread_mutex_unlock(&counter->local_locks[i]);
    }
    
    return total;
}

// Clean up the counter
void sloppy_counter_destroy(sloppy_counter_t *counter) {
    pthread_mutex_destroy(&counter->global_lock);
    for (int i = 0; i < counter->num_cpus; i++) {
        pthread_mutex_destroy(&counter->local_locks[i]);
    }
    free(counter->local_counts);
    free(counter->local_locks);
}

// Thread function
void *thread_func(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    
    // Pin thread to specific CPU core if possible
    for (int i = 0; i < args->increments; i++) {
        sloppy_counter_update(args->counter, args->cpu_core);
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
    sloppy_counter_init(&counter);

    pthread_t *threads = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
    thread_args_t *thread_args = (thread_args_t *)malloc(num_threads * sizeof(thread_args_t));

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Create threads
    for (int i = 0; i < num_threads; i++) {
        thread_args[i].counter = &counter;
        thread_args[i].thread_id = i;
        thread_args[i].increments = increments_per_thread;
        thread_args[i].cpu_core = i % counter.num_cpus; // Map to CPU core
        
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

    // Get final count
    int final_count = sloppy_counter_get(&counter);

    printf("Time taken: %.2f ms\n", elapsed);
    printf("Final counter value: %d\n", final_count);
    printf("Expected value: %d\n", num_threads * increments_per_thread);

    // Clean up
    sloppy_counter_destroy(&counter);
    free(threads);
    free(thread_args);

    return 0;
}