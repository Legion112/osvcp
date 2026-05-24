#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>

// Глобальные переменные
pthread_mutex_t mutex;
unsigned long long counter = 0;

// Структура для передачи параметров потоку
typedef struct {
    int increments;
} thread_args;

// Функция потока
void* thread_function(void* arg) {
    thread_args* args = (thread_args*)arg;

    for(int i = 0; i < args->increments; i++) {
        pthread_mutex_lock(&mutex);
        counter++;
        pthread_mutex_unlock(&mutex);
    }

    pthread_exit(NULL);
}

// Функция для измерения времени
double get_current_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

int main(int argc, char* argv[]) {
    if(argc != 3) {
        fprintf(stderr, "Usage: %s <num_threads> <increments_per_thread>\n", argv[0]);
        return 1;
    }

    int num_threads = atoi(argv[1]);
    int increments = atoi(argv[2]);

    // Инициализация мьютекса
    pthread_mutex_init(&mutex, NULL);

    pthread_t* threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t));
    thread_args* args = (thread_args*)malloc(num_threads * sizeof(thread_args));

    // Замеряем время
    double start_time = get_current_time();

    // Создаем потоки
    for(int i = 0; i < num_threads; i++) {
        args[i].increments = increments;
        pthread_create(&threads[i], NULL, thread_function, &args[i]);
    }

    // Ждем завершения всех потоков
    for(int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    double end_time = get_current_time();

    // Вывод результатов
    printf("Final counter value: %llu\n", counter);
    printf("Elapsed time: %.6f seconds\n", end_time - start_time);

    // Очистка ресурсов
    pthread_mutex_destroy(&mutex);
    free(threads);
    free(args);

    return 0;
}
