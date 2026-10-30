#ifndef __OOSDK_FIXUPS_H__
#define __OOSDK_FIXUPS_H__

/* Force-included in every translation unit of the OpenOrbis build.
 *
 * OpenOrbis' musl headers use Linux clock ids, but the PS4 kernel is
 * FreeBSD: id 1 there is CLOCK_VIRTUAL (process CPU time), which stands
 * still while the process sleeps. Use FreeBSD's CLOCK_MONOTONIC. */
#include <time.h>
#undef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 4

#endif
