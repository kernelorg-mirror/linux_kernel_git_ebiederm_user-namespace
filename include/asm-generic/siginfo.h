#ifndef _ASM_GENERIC_SIGINFO_H
#define _ASM_GENERIC_SIGINFO_H

#include <uapi/asm-generic/siginfo.h>

enum siginfo_layout {
	SIL_KILL,
	SIL_TIMER,
	SIL_POLL,
	SIL_FAULT,
	SIL_CHLD,
	SIL_RT,
#ifdef __ARCH_SIGSYS
	SIL_SYS,
#endif
};

struct siginfo;
void do_schedule_next_timer(struct siginfo *info);

extern int copy_siginfo_to_user(struct siginfo __user *to, const struct siginfo *from);
extern enum siginfo_layout siginfo_layout(int sig, int si_code);

#endif
