#pragma once

#include <bits/pthreadtypes.h>
#include <stdbool.h>

enum parallel_interrupt {
	// no interrupt currently
	PNONE,

	// which interrupt to trigger next
	PAR0,
	PAR1,
	PAR2,
};

extern void parallel_init(void);

// output pipes for p0, p1, p2; read by the vm when taking input
extern int p0_fd, p1_fd, p2_fd;
