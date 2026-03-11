/*
 * thread.c - Thread spawn/await for Clay
 *
 * Thread spawn(block) → Future
 * future await() → result (blocks until done)
 *
 * Requires -DCLAY_THREAD_SAFE build for atomic refcounts.
 * Each worker thread gets its own VM context (thread-local g_bc, g_vm).
 */
#include "clay.h"
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* Callback to initialize a worker thread's VM context.
 * Set by the compiler (bc_compiler.c) at startup. */
static void (*g_thread_vm_init)(void) = NULL;

void clay_thread_set_vm_init(void (*fn)(void)) {
    g_thread_vm_init = fn;
}

#ifdef CLAY_THREAD_SAFE

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

/* ---- Future internal state ---- */
typedef struct {
    ClayValue    result;
    volatile int done;
#ifdef _WIN32
    HANDLE       thread_handle;
    CRITICAL_SECTION lock;
#else
    pthread_t    thread_id;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
#endif
    ClayObject  *block;  /* retained reference to the block to execute */
} FutureData;

static void future_data_free(void *data) {
    FutureData *fd = (FutureData *)data;
    if (!fd) return;
    if (CLAY_IS_PTR(fd->result) && !CLAY_IS_NIL(fd->result) &&
        fd->result != CLAY_TRUE && fd->result != CLAY_FALSE) {
        clay_release(CLAY_OBJ(fd->result));
    }
    if (fd->block) clay_release(fd->block);
#ifdef _WIN32
    if (fd->thread_handle) CloseHandle(fd->thread_handle);
    DeleteCriticalSection(&fd->lock);
#else
    pthread_mutex_destroy(&fd->lock);
    pthread_cond_destroy(&fd->cond);
#endif
    free(fd);
}

/* ---- Worker thread entry ---- */
#ifdef _WIN32
static unsigned __stdcall thread_worker(void *arg) {
#else
static void *thread_worker(void *arg) {
#endif
    FutureData *fd = (FutureData *)arg;

    /* Each worker thread needs its own VM context */
    if (g_thread_vm_init) {
        g_thread_vm_init();
    }

    /* Execute the block */
    ClayValue result = clay_send(CLAY_OBJ(CLAY_VAL(fd->block)), "value", 0, NULL);

    /* Retain result so it survives thread cleanup */
    if (CLAY_IS_PTR(result) && !CLAY_IS_NIL(result) &&
        result != CLAY_TRUE && result != CLAY_FALSE) {
        clay_retain(CLAY_OBJ(result));
    }

    /* Store result and signal completion */
#ifdef _WIN32
    EnterCriticalSection(&fd->lock);
    fd->result = result;
    fd->done = 1;
    LeaveCriticalSection(&fd->lock);
#else
    pthread_mutex_lock(&fd->lock);
    fd->result = result;
    fd->done = 1;
    pthread_cond_signal(&fd->cond);
    pthread_mutex_unlock(&fd->lock);
#endif

    /* Clean up worker VM (but not the globals — those are shared) */
    /* Note: bc_compiler_cleanup would free chunks, but worker doesn't own any */

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- Thread methods ---- */

/* Thread spawn(block) → Future */
static ClayValue thread_spawn(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || CLAY_IS_NIL(argv[0])) {
        clay_raise(CLAY_VAL(clay_error_new("Thread spawn requires a block", "TypeError")));
        return CLAY_NIL;
    }

    ClayObject *block = CLAY_OBJ(argv[0]);
    clay_retain(block);

    /* Create Future as a plugin-like object with custom data */
    ClayObject *future = clay_clone(clay_Thread);
    future->type = CLAY_TYPE_PLUGIN;

    FutureData *fd = (FutureData *)calloc(1, sizeof(FutureData));
    fd->block = block;
    fd->result = CLAY_NIL;
    fd->done = 0;

#ifdef _WIN32
    InitializeCriticalSection(&fd->lock);
#else
    pthread_mutex_init(&fd->lock, NULL);
    pthread_cond_init(&fd->cond, NULL);
#endif

    future->as.plugin.data = fd;
    future->as.plugin.free_fn = future_data_free;

    /* Spawn the worker thread */
#ifdef _WIN32
    fd->thread_handle = (HANDLE)_beginthreadex(NULL, 0, thread_worker, fd, 0, NULL);
    if (!fd->thread_handle) {
        clay_raise(CLAY_VAL(clay_error_new("Failed to create thread", "RuntimeError")));
        return CLAY_NIL;
    }
#else
    if (pthread_create(&fd->thread_id, NULL, thread_worker, fd) != 0) {
        clay_raise(CLAY_VAL(clay_error_new("Failed to create thread", "RuntimeError")));
        return CLAY_NIL;
    }
#endif

    return CLAY_VAL(future);
}

/* future await() → result */
static ClayValue thread_await(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->type != CLAY_TYPE_PLUGIN || !self->as.plugin.data) {
        clay_raise(CLAY_VAL(clay_error_new("await called on non-future", "TypeError")));
        return CLAY_NIL;
    }

    FutureData *fd = (FutureData *)self->as.plugin.data;

#ifdef _WIN32
    /* Wait for thread to finish */
    if (fd->thread_handle) {
        WaitForSingleObject(fd->thread_handle, INFINITE);
    }
#else
    pthread_mutex_lock(&fd->lock);
    while (!fd->done) {
        pthread_cond_wait(&fd->cond, &fd->lock);
    }
    pthread_mutex_unlock(&fd->lock);

    /* Join the thread to clean up resources */
    pthread_join(fd->thread_id, NULL);
#endif

    ClayValue result = fd->result;
    if (CLAY_IS_PTR(result) && !CLAY_IS_NIL(result) &&
        result != CLAY_TRUE && result != CLAY_FALSE) {
        clay_retain(CLAY_OBJ(result));
    }
    return result;
}

/* future done() → true/false */
static ClayValue thread_done(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->type != CLAY_TYPE_PLUGIN || !self->as.plugin.data) return CLAY_FALSE;
    FutureData *fd = (FutureData *)self->as.plugin.data;
    return CLAY_BOOL(fd->done);
}

static ClayValue thread_available(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return CLAY_TRUE;
}

void clay_thread_init(ClayObject *proto) {
    clay_set_method(proto, "spawn", thread_spawn);
    clay_set_method(proto, "await", thread_await);
    clay_set_method(proto, "done", thread_done);
    clay_set_method(proto, "available", thread_available);
}

/* ---- parallelMap: chunked worker ---- */
typedef struct {
    ClayObject  *block;   /* the mapper block (shared, retained) */
    ClayValue    item;    /* input element (retained) */
    ClayValue    result;  /* output element */
    volatile int done;
} PMapWorker;

typedef struct {
    PMapWorker *workers;
    size_t start;
    size_t end;
    ClayObject *block;
} PMapChunkArg;

#ifdef _WIN32
static unsigned __stdcall pmap_chunk_worker(void *arg) {
#else
static void *pmap_chunk_worker(void *arg) {
#endif
    PMapChunkArg *ca = (PMapChunkArg *)arg;

    if (g_thread_vm_init) g_thread_vm_init();

    for (size_t i = ca->start; i < ca->end; i++) {
        PMapWorker *w = &ca->workers[i];
        ClayValue result = clay_send(w->block, "value", 1, &w->item);
        if (CLAY_IS_PTR(result) && !CLAY_IS_NIL(result) &&
            result != CLAY_TRUE && result != CLAY_FALSE)
            clay_retain(CLAY_OBJ(result));
        w->result = result;
        w->done = 1;
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

ClayValue clay_parallel_map(ClayObject *arr, ClayObject *block) {
    size_t n = arr->as.array.size;
    if (n == 0) return CLAY_VAL(clay_array_new());

    /* Cap thread count to avoid excessive OS thread creation.
     * Each thread processes a contiguous chunk of the array. */
    size_t max_threads = 8;  /* reasonable default for most machines */
#ifdef _WIN32
    {
        /* Minimal declarations to avoid including windows.h name clashes */
        typedef struct { uintptr_t mask; uintptr_t group; } SYSTEM_INFO_AFFINITY;
        typedef struct {
            union { unsigned long oemId; struct { unsigned short arch; unsigned short _; } s; } u;
            unsigned long pageSize;
            void *minAddr, *maxAddr;
            uintptr_t activeMask;
            unsigned long numProcs, procType, allocGran;
            unsigned short procLevel, procRev;
        } CLAY_SYSINFO;
        __declspec(dllimport) void __stdcall GetSystemInfo(CLAY_SYSINFO *);
        CLAY_SYSINFO si;
        GetSystemInfo(&si);
        if (si.numProcs > 1) max_threads = si.numProcs;
    }
#else
    {
        long nproc = sysconf(_SC_NPROCESSORS_ONLN);
        if (nproc > 1) max_threads = (size_t)nproc;
    }
#endif
    size_t num_threads = n < max_threads ? n : max_threads;

    PMapWorker *workers = (PMapWorker *)calloc(n, sizeof(PMapWorker));
#ifdef _WIN32
    HANDLE *handles = (HANDLE *)calloc(num_threads, sizeof(HANDLE));
#else
    pthread_t *threads = (pthread_t *)calloc(num_threads, sizeof(pthread_t));
#endif

    /* Set up all work items */
    clay_retain(block);
    for (size_t i = 0; i < n; i++) {
        workers[i].block = block;
        workers[i].item = arr->as.array.items[i];
        workers[i].result = CLAY_NIL;
        workers[i].done = 0;

        ClayValue item = workers[i].item;
        if (CLAY_IS_PTR(item) && !CLAY_IS_NIL(item) &&
            item != CLAY_TRUE && item != CLAY_FALSE)
            clay_retain(CLAY_OBJ(item));
    }

    /* Assign work ranges to threads: each thread processes [start, end) */
    PMapChunkArg *chunks = (PMapChunkArg *)calloc(num_threads, sizeof(PMapChunkArg));
    size_t items_per = n / num_threads;
    size_t extra = n % num_threads;
    size_t offset = 0;
    for (size_t t = 0; t < num_threads; t++) {
        chunks[t].workers = workers;
        chunks[t].start = offset;
        chunks[t].end = offset + items_per + (t < extra ? 1 : 0);
        chunks[t].block = block;
        offset = chunks[t].end;
    }

    /* Launch worker threads */
    for (size_t t = 0; t < num_threads; t++) {
#ifdef _WIN32
        handles[t] = (HANDLE)_beginthreadex(NULL, 0, pmap_chunk_worker, &chunks[t], 0, NULL);
#else
        pthread_create(&threads[t], NULL, pmap_chunk_worker, &chunks[t]);
#endif
    }

    /* Wait for all threads */
#ifdef _WIN32
    for (size_t t = 0; t < num_threads; t++) {
        WaitForSingleObject(handles[t], INFINITE);
        CloseHandle(handles[t]);
    }
    free(handles);
#else
    for (size_t t = 0; t < num_threads; t++) pthread_join(threads[t], NULL);
    free(threads);
#endif
    free(chunks);

    /* Collect results */
    ClayObject *result = clay_array_new();
    for (size_t i = 0; i < n; i++) {
        clay_array_append(result, workers[i].result);
        /* Release item */
        ClayValue item = workers[i].item;
        if (CLAY_IS_PTR(item) && !CLAY_IS_NIL(item) &&
            item != CLAY_TRUE && item != CLAY_FALSE)
            clay_release(CLAY_OBJ(item));
    }

    clay_release(block);
    free(workers);
    return CLAY_VAL(result);
}

#else /* !CLAY_THREAD_SAFE */

/* Sequential fallback for parallelMap */
ClayValue clay_parallel_map(ClayObject *arr, ClayObject *block) {
    ClayObject *result = clay_array_new();
    for (size_t i = 0; i < arr->as.array.size; i++) {
        ClayValue item = arr->as.array.items[i];
        ClayValue mapped = clay_send(block, "value", 1, &item);
        clay_array_append(result, mapped);
        clay_release_val(mapped);
    }
    return CLAY_VAL(result);
}

static ClayValue thread_not_available(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    clay_raise(CLAY_VAL(clay_error_new(
        "Threads not available — rebuild with -DCLAY_THREAD_SAFE",
        "RuntimeError")));
    return CLAY_NIL;
}

static ClayValue thread_available_false(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return CLAY_FALSE;
}

void clay_thread_init(ClayObject *proto) {
    clay_set_method(proto, "spawn", thread_not_available);
    clay_set_method(proto, "available", thread_available_false);
}

#endif /* CLAY_THREAD_SAFE */
