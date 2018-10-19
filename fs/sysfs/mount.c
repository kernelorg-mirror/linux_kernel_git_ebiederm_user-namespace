// SPDX-License-Identifier: GPL-2.0
/*
 * fs/sysfs/symlink.c - operations for initializing and mounting sysfs
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/init.h>
#include <linux/user_namespace.h>
#include <net/net_namespace.h>

#include "sysfs.h"

static struct kernfs_root *sysfs_root;
struct kernfs_node *sysfs_root_kn;

static struct dentry *sysfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	struct net *net = current->nsproxy->net_ns;
	struct dentry *root;
	bool new_sb = false;

	if (!(flags & SB_KERNMOUNT)) {
		if (!ns_capable(net->user_ns, CAP_SYS_ADMIN))
			return ERR_PTR(-EPERM);
	}

	net = hold_net(net);
	root = kernfs_mount_ns(fs_type, flags, sysfs_root,
				SYSFS_MAGIC, &new_sb, net);
	if (!new_sb)
		drop_net(net);
	else if (!IS_ERR(root))
		root->d_sb->s_iflags |= SB_I_USERNS_VISIBLE;

	return root;
}

static void sysfs_kill_sb(struct super_block *sb)
{
	void *ns = (void *)kernfs_super_ns(sb);

	kernfs_kill_sb(sb);
	drop_net(ns);
}

static struct file_system_type sysfs_fs_type = {
	.name		= "sysfs",
	.mount		= sysfs_mount,
	.kill_sb	= sysfs_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

int __init sysfs_init(void)
{
	int err;

	sysfs_root = kernfs_create_root(NULL, KERNFS_ROOT_EXTRA_OPEN_PERM_CHECK,
					NULL);
	if (IS_ERR(sysfs_root))
		return PTR_ERR(sysfs_root);

	sysfs_root_kn = sysfs_root->kn;

	err = register_filesystem(&sysfs_fs_type);
	if (err) {
		kernfs_destroy_root(sysfs_root);
		return err;
	}

	return 0;
}
