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

#define DEVPTS_DEFAULT_PTMX_MODE 0666
#define PTMX_MINOR	2

#ifdef CONFIG_UNIX98_PTYS
#include <linux/major.h>

extern struct file_operations ptmx_fops;

struct vfsmount *ptmx_automount(struct path *path);

int devpts_new_index(struct inode *ptmx_inode);
void devpts_kill_index(struct inode *ptmx_inode, int idx);
void devpts_add_ref(struct inode *ptmx_inode);
void devpts_del_ref(struct inode *ptmx_inode);
/* mknod in devpts */
struct inode *devpts_pty_new(struct inode *ptmx_inode, dev_t device, int index,
		void *priv);
/* get private structure */
void *devpts_get_priv(struct inode *pts_inode);
/* unlink */
void devpts_pty_kill(struct inode *inode);

static inline bool is_dev_ptmx(struct inode *inode)
{
	return inode->i_rdev == MKDEV(TTYAUX_MAJOR, PTMX_MINOR);
}
#else

/* Dummy stubs in the no-pty case */
static inline int devpts_new_index(struct inode *ptmx_inode) { return -EINVAL; }
static inline void devpts_kill_index(struct inode *ptmx_inode, int idx) { }
static inline void devpts_add_ref(struct inode *ptmx_inode) { }
static inline void devpts_del_ref(struct inode *ptmx_inode) { }
static inline struct inode *devpts_pty_new(struct inode *ptmx_inode,
		dev_t device, int index, void *priv)
{
	return ERR_PTR(-EINVAL);
}
static inline void *devpts_get_priv(struct inode *pts_inode)
{
	return NULL;
}
static inline void devpts_pty_kill(struct inode *inode) { }

#define ptmx_automount NULL

static inline bool is_dev_ptmx(struct inode *inode)
{
	return false;
}
#endif


#endif /* _LINUX_DEVPTS_FS_H */
