#pragma once

#include <bits/pthreadtypes.h>
enum interrupt_type {
	INT_NONE,

	INT_P0,
	INT_P1,
	INT_P2,
};

extern enum interrupt_type current_interrupt;
extern pthread_cond_t interrupt_cond;
extern pthread_mutex_t interrupt_cond_mutex;
