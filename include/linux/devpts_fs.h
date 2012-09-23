/* -*- linux-c -*- --------------------------------------------------------- *
 *
 * linux/include/linux/devpts_fs.h
 *
 *  Copyright 1998-2004 H. Peter Anvin -- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * ------------------------------------------------------------------------- */

#ifndef _LINUX_DEVPTS_FS_H
#define _LINUX_DEVPTS_FS_H

#include <linux/errno.h>
#include <linux/mount.h>

#define DEVPTS_DEFAULT_PTMX_MODE 0666
#define PTMX_MINOR	2

#ifdef CONFIG_UNIX98_PTYS

struct vfsmount *devpts_mntget(struct file *filp);
int devpts_new_index(struct vfsmount *mnt);
void devpts_kill_index(struct vfsmount *mnt, int idx);
/* mknod in devpts */
int devpts_pty_new(struct vfsmount *mnt, struct tty_struct *tty);
/* get tty structure */
struct tty_struct *devpts_get_tty(struct inode *pts_inode, int number);
/* unlink */
void devpts_pty_kill(struct tty_struct *tty);

#else

/* Dummy stubs in the no-pty case */
static inline struct tty_struct *devpts_get_tty(struct inode *pts_inode,
		int number)
{
	return NULL;
}
static inline void devpts_pty_kill(struct tty_struct *tty) { }

#endif


#endif /* _LINUX_DEVPTS_FS_H */
