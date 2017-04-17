/*
 *  include/linux/signalfd.h
 *
 *  Copyright (C) 2007  Davide Libenzi <davidel@xmailserver.org>
 *
 */
#ifndef _LINUX_SIGNALFD_H
#define _LINUX_SIGNALFD_H

#include <uapi/linux/signalfd.h>
#include <linux/sched/signal.h>

#ifdef CONFIG_SIGNALFD

/*
 * Deliver the signal to listening signalfd.
 */
static inline void signalfd_notify(struct task_struct *tsk, int sig)
{
	if (unlikely(waitqueue_active(&tsk->signal->signalfd_wqh)))
		wake_up(&tsk->signal->signalfd_wqh);
}

extern void signalfd_cleanup(struct signal_struct *sig);

#else /* CONFIG_SIGNALFD */

static inline void signalfd_notify(struct task_struct *tsk, int sig) { }

static inline void signalfd_cleanup(struct signal_struct *sig) { }

#endif /* CONFIG_SIGNALFD */

#endif /* _LINUX_SIGNALFD_H */
