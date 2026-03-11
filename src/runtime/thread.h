/*
 * thread.h - Thread spawn/await API
 */
#ifndef CLAY_THREAD_H
#define CLAY_THREAD_H

#include "clay.h"

extern ClayObject *clay_Thread;

void clay_thread_init(ClayObject *proto);

#endif
