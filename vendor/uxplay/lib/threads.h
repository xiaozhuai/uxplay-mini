/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *=================================================================
 * modified by fduncanh 2022
 */

#ifndef THREADS_H
#define THREADS_H

/* Use native Windows synchronization; no external pthread port is needed. */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

typedef HANDLE thread_handle_t;
typedef CRITICAL_SECTION mutex_handle_t;
typedef CONDITION_VARIABLE cond_handle_t;
typedef struct { void *(*func)(void *); void *arg; } mini_thread_start_t;
static unsigned __stdcall mini_thread_entry(void *opaque) {
    mini_thread_start_t start = *(mini_thread_start_t *)opaque;
    free(opaque);
    start.func(start.arg);
    return 0;
}
static thread_handle_t mini_thread_create(void *(*func)(void *), void *arg) {
    mini_thread_start_t *start = malloc(sizeof(*start));
    if (!start) return NULL;
    start->func = func; start->arg = arg;
    HANDLE handle = (HANDLE)_beginthreadex(NULL, 0, mini_thread_entry, start, 0, NULL);
    if (!handle) free(start);
    return handle;
}
static void mini_thread_join(HANDLE handle) {
    if (handle) { WaitForSingleObject(handle, INFINITE); CloseHandle(handle); }
}
static void mini_walltime(struct timespec *time) {
    FILETIME ft; ULARGE_INTEGER ticks;
    GetSystemTimeAsFileTime(&ft);
    ticks.LowPart = ft.dwLowDateTime; ticks.HighPart = ft.dwHighDateTime;
    uint64_t unix_ticks = ticks.QuadPart - UINT64_C(116444736000000000);
    time->tv_sec = (time_t)(unix_ticks / 10000000);
    time->tv_nsec = (long)((unix_ticks % 10000000) * 100);
}
static void mini_cond_wait(cond_handle_t *cond, mutex_handle_t *mutex, const struct timespec *end) {
    struct timespec now; mini_walltime(&now);
    int64_t ms = ((int64_t)end->tv_sec - now.tv_sec) * 1000 + (end->tv_nsec - now.tv_nsec + 999999) / 1000000;
    SleepConditionVariableCS(cond, mutex, ms <= 0 ? 0 : ms > MAXDWORD - 1 ? MAXDWORD - 1 : (DWORD)ms);
}
#define sleepms(x) Sleep(x)
#define THREAD_RETVAL void *
#define THREAD_CREATE(handle, func, arg) ((handle) = mini_thread_create(func, arg))
#define THREAD_JOIN(handle) mini_thread_join(handle)
#define MUTEX_CREATE(handle) InitializeCriticalSection(&(handle))
#define MUTEX_LOCK(handle) EnterCriticalSection(&(handle))
#define MUTEX_UNLOCK(handle) LeaveCriticalSection(&(handle))
#define MUTEX_DESTROY(handle) DeleteCriticalSection(&(handle))
#define COND_CREATE(handle) InitializeConditionVariable(&(handle))
#define COND_SIGNAL(handle) WakeConditionVariable(&(handle))
#define COND_DESTROY(handle) ((void)0)
#define COND_TIMEDWAIT(cond, mutex, end) mini_cond_wait(&(cond), &(mutex), end)
#else

#include <pthread.h>
#include <unistd.h>
#include <time.h>

static void mini_walltime(struct timespec *time) { clock_gettime(CLOCK_REALTIME, time); }

#define sleepms(x) usleep((x)*1000)

typedef pthread_t thread_handle_t;

#define THREAD_RETVAL void *
#define THREAD_CREATE(handle, func, arg) \
	if (pthread_create(&(handle), NULL, func, arg)) handle = 0
#define THREAD_JOIN(handle) pthread_join(handle, NULL)

typedef pthread_mutex_t mutex_handle_t;

typedef pthread_cond_t cond_handle_t;

#define MUTEX_CREATE(handle) pthread_mutex_init(&(handle), NULL)
#define MUTEX_LOCK(handle) pthread_mutex_lock(&(handle))
#define MUTEX_UNLOCK(handle) pthread_mutex_unlock(&(handle))
#define MUTEX_DESTROY(handle) pthread_mutex_destroy(&(handle))

#define COND_CREATE(handle) pthread_cond_init(&(handle), NULL)
#define COND_SIGNAL(handle) pthread_cond_signal(&(handle))
#define COND_DESTROY(handle) pthread_cond_destroy(&(handle))
#define COND_TIMEDWAIT(cond, mutex, end) pthread_cond_timedwait(&(cond), &(mutex), end)
#endif

#endif /* THREADS_H */
