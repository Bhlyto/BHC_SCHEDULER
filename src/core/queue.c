#include "queue.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
typedef CRITICAL_SECTION  mutex_t;
typedef CONDITION_VARIABLE cond_t;
#  define mutex_init(m)    InitializeCriticalSection(m)
#  define mutex_lock(m)    EnterCriticalSection(m)
#  define mutex_unlock(m)  LeaveCriticalSection(m)
#  define cond_init(c)     InitializeConditionVariable(c)
#  define cond_wait(c,m)   SleepConditionVariableCS(c, m, INFINITE)
#  define cond_signal(c)   WakeConditionVariable(c)
#  define cond_broadcast(c) WakeAllConditionVariable(c)
#else
#  include <pthread.h>
typedef pthread_mutex_t mutex_t;
typedef pthread_cond_t  cond_t;
#  define mutex_init(m)    pthread_mutex_init(m, NULL)
#  define mutex_lock(m)    pthread_mutex_lock(m)
#  define mutex_unlock(m)  pthread_mutex_unlock(m)
#  define cond_init(c)     pthread_cond_init(c, NULL)
#  define cond_wait(c,m)   pthread_cond_wait(c, m)
#  define cond_signal(c)   pthread_cond_signal(c)
#  define cond_broadcast(c) pthread_cond_broadcast(c)
#endif

/*
 * queue.c
 * Min-heap priority queue (lower priority number = dequeued first).
 * Fully thread-safe.
 */

struct Queue {
    Job    **heap;
    int      size;
    int      capacity;
    int      shutdown;
    mutex_t  lock;
    cond_t   not_empty;
};

static void heap_swap(Queue *q, int a, int b)
{
    Job *tmp = q->heap[a]; q->heap[a] = q->heap[b]; q->heap[b] = tmp;
}

static void heap_up(Queue *q, int i)
{
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (q->heap[parent]->priority <= q->heap[i]->priority) break;
        heap_swap(q, parent, i);
        i = parent;
    }
}

static void heap_down(Queue *q, int i)
{
    int n = q->size;
    while (1) {
        int smallest = i;
        int l = 2*i+1, r = 2*i+2;
        if (l < n && q->heap[l]->priority < q->heap[smallest]->priority) smallest = l;
        if (r < n && q->heap[r]->priority < q->heap[smallest]->priority) smallest = r;
        if (smallest == i) break;
        heap_swap(q, i, smallest);
        i = smallest;
    }
}

Queue *queue_create(int initial_capacity)
{
    Queue *q = (Queue *)calloc(1, sizeof(Queue));
    if (!q) return NULL;
    if (initial_capacity < 8) initial_capacity = 8;
    q->heap     = (Job **)malloc(sizeof(Job *) * initial_capacity);
    q->capacity = initial_capacity;
    q->size     = 0;
    q->shutdown = 0;
    mutex_init(&q->lock);
    cond_init(&q->not_empty);
    return q;
}

void queue_destroy(Queue *q)
{
    if (!q) return;
    free(q->heap);
    free(q);
}

int queue_push(Queue *q, Job *job)
{
    mutex_lock(&q->lock);
    if (q->size == q->capacity) {
        int new_cap = q->capacity * 2;
        Job **new_heap = (Job **)realloc(q->heap, sizeof(Job *) * new_cap);
        if (!new_heap) { mutex_unlock(&q->lock); return -1; }
        q->heap = new_heap;
        q->capacity = new_cap;
    }
    q->heap[q->size++] = job;
    heap_up(q, q->size - 1);
    cond_signal(&q->not_empty);
    mutex_unlock(&q->lock);
    log_debug("queue", "Pushed job %s (priority=%d), queue size=%d",
              job->id, job->priority, q->size);
    return 0;
}

Job *queue_pop(Queue *q)
{
    mutex_lock(&q->lock);
    while (q->size == 0 && !q->shutdown)
        cond_wait(&q->not_empty, &q->lock);
    if (q->shutdown && q->size == 0) { mutex_unlock(&q->lock); return NULL; }
    Job *job = q->heap[0];
    q->heap[0] = q->heap[--q->size];
    if (q->size > 0) heap_down(q, 0);
    mutex_unlock(&q->lock);
    return job;
}

Job *queue_try_pop(Queue *q)
{
    mutex_lock(&q->lock);
    if (q->size == 0) { mutex_unlock(&q->lock); return NULL; }
    Job *job = q->heap[0];
    q->heap[0] = q->heap[--q->size];
    if (q->size > 0) heap_down(q, 0);
    mutex_unlock(&q->lock);
    return job;
}

int queue_size(Queue *q)
{
    mutex_lock(&q->lock);
    int n = q->size;
    mutex_unlock(&q->lock);
    return n;
}

void queue_shutdown(Queue *q)
{
    mutex_lock(&q->lock);
    q->shutdown = 1;
    cond_broadcast(&q->not_empty);
    mutex_unlock(&q->lock);
}
