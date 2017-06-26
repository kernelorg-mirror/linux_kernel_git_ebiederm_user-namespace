#ifndef _ALPHA_SIGINFO_H
#define _ALPHA_SIGINFO_H

#define __ARCH_SI_PREAMBLE_SIZE		(4 * sizeof(int))
#define __ARCH_SI_TRAPNO

#include <asm-generic/siginfo.h>

/*
 * SIGTRAP si_codes
 */
#define TRAP_FIXME	(__SI_FAULT|0)	/* Broken dup of SI_USER */

#endif
