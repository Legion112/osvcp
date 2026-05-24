#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "common_threads.h"

//
// Here, you have to write (almost) ALL the code. Oh no!
// How can you show that a thread does not starve
// when attempting to acquire this mutex you build?
//

typedef struct __node_t {
    sem_t sem;
    struct __node_t *next;
} node_t;

typedef struct __ns_mutex_t {
    int  locked;
    node_t *head, *tail;
    sem_t guard;
} ns_mutex_t;

void ns_mutex_init(ns_mutex_t *m) {
    m->locked = 0;
    m->head = NULL;
    m->tail = NULL;
    Sem_init(&m->guard, 1);
}

void ns_mutex_acquire(ns_mutex_t *m) {
    node_t *n = (node_t *) malloc(sizeof(node_t));
    Sem_init(&n->sem, 0);
    n->next = NULL;

    Sem_wait(&m->guard);
    if (m->locked == 0) {
        m->locked = 1;
        Sem_post(&m->guard);
        free(n);
    } else {
        // append to queue
        if (m->tail) {
            m->tail->next = n;
        } else {
            m->head = n;
        }
        m->tail = n;
        Sem_post(&m->guard);
        Sem_wait(&n->sem);
        free(n);
    }
}

void ns_mutex_release(ns_mutex_t *m) {
    Sem_wait(&m->guard);
    if (m->head == NULL) {
        m->locked = 0;
    } else {
        node_t *next = m->head;
        m->head  = next->next;
        Sem_post(&next->sem);
    }
    Sem_post(&m->guard);
}


void *worker(void *arg) {
    ns_mutex_t *m = (ns_mutex_t *) arg;
    ns_mutex_acquire(m);
    printf("thread %lu: acquired\n", pthread_self());
    sleep(1);
    ns_mutex_release(m);
    printf("thread %lu: released\n", pthread_self());
    return NULL;
}

int main(int argc, char *argv[]) {
    assert(argc == 2);
    int num_threads = atoi(argv[1]);
    pthread_t p[num_threads];
    ns_mutex_t m;
    ns_mutex_init(&m);


    printf("parent: begin\n");
    for (int i = 0; i < num_threads; i++) {
        Pthread_create(&p[i], NULL, worker, &m);
    }
    for (int i = 0; i < num_threads; i++) {
        Pthread_join(p[i], NULL);
    }

    printf("parent: end\n");
    return 0;
}

