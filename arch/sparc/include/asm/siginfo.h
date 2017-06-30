#ifndef __SPARC_SIGINFO_H
#define __SPARC_SIGINFO_H

#include <uapi/asm/siginfo.h>

/*
 * SIGFPE si_codes
 */
#define FPE_FIXME	0	/* Broken dup of SI_USER */

#ifdef CONFIG_COMPAT

struct compat_siginfo;

#endif /* CONFIG_COMPAT */

#endif /* !(__SPARC_SIGINFO_H) */
