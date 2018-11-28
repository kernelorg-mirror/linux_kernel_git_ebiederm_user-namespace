/* mountpoint management
 *
 * Copyright (C) 2002 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/gfp.h>
#include "internal.h"


static struct dentry *afs_mntpt_lookup(struct inode *dir,
				       struct dentry *dentry,
				       unsigned int flags);
static int afs_mntpt_open(struct inode *inode, struct file *file);

const struct file_operations afs_mntpt_file_operations = {
	.open		= afs_mntpt_open,
	.llseek		= noop_llseek,
};

const struct inode_operations afs_mntpt_inode_operations = {
	.lookup		= afs_mntpt_lookup,
	.readlink	= page_readlink,
	.getattr	= afs_getattr,
	.listxattr	= afs_listxattr,
};

const struct inode_operations afs_autocell_inode_operations = {
	.getattr	= afs_getattr,
};

/*
 * no valid lookup procedure on this sort of dir
 */
static struct dentry *afs_mntpt_lookup(struct inode *dir,
				       struct dentry *dentry,
				       unsigned int flags)
{
	_enter("%p,%p{%pd2}", dir, dentry, dentry);
	return ERR_PTR(-EREMOTE);
}

/*
 * no valid open procedure on this sort of dir
 */
static int afs_mntpt_open(struct inode *inode, struct file *file)
{
	_enter("%p,%p{%pD2}", inode, file, file);
	return -EREMOTE;
}

/*
 * handle an automount point
 */
struct dentry *afs_d_automount(struct path *path)
{
	struct dentry *mntpt = path->dentry;
	struct afs_super_info *as;
	struct dentry *root;
	struct afs_vnode *vnode;
	struct page *page;
	char *devname, *options;
	bool rwpath = false;
	int ret;

	_enter("{%pd}", mntpt);

	BUG_ON(!d_inode(mntpt));

	ret = -ENOMEM;
	devname = (char *) get_zeroed_page(GFP_KERNEL);
	if (!devname)
		goto error_no_devname;

	options = (char *) get_zeroed_page(GFP_KERNEL);
	if (!options)
		goto error_no_options;

	vnode = AFS_FS_I(d_inode(mntpt));
	if (test_bit(AFS_VNODE_PSEUDODIR, &vnode->flags)) {
		/* if the directory is a pseudo directory, use the d_name */
		static const char afs_root_cell[] = ":root.cell.";
		unsigned size = mntpt->d_name.len;

		ret = -ENOENT;
		if (size < 2 || size > AFS_MAXCELLNAME)
			goto error_no_page;

		if (mntpt->d_name.name[0] == '.') {
			devname[0] = '%';
			memcpy(devname + 1, mntpt->d_name.name + 1, size - 1);
			memcpy(devname + size, afs_root_cell,
			       sizeof(afs_root_cell));
			rwpath = true;
		} else {
			devname[0] = '#';
			memcpy(devname + 1, mntpt->d_name.name, size);
			memcpy(devname + size + 1, afs_root_cell,
			       sizeof(afs_root_cell));
		}
	} else {
		/* read the contents of the AFS special symlink */
		loff_t size = i_size_read(d_inode(mntpt));
		char *buf;

		ret = -EINVAL;
		if (size > PAGE_SIZE - 1)
			goto error_no_page;

		page = read_mapping_page(d_inode(mntpt)->i_mapping, 0, NULL);
		if (IS_ERR(page)) {
			ret = PTR_ERR(page);
			goto error_no_page;
		}

		if (PageError(page)) {
			ret = afs_bad(AFS_FS_I(d_inode(mntpt)), afs_file_error_mntpt);
			goto error;
		}

		buf = kmap_atomic(page);
		memcpy(devname, buf, size);
		kunmap_atomic(buf);
		put_page(page);
		page = NULL;
	}

	/* work out what options we want */
	as = AFS_FS_S(mntpt->d_sb);
	if (as->cell) {
		memcpy(options, "cell=", 5);
		strcpy(options + 5, as->cell->name);
		if ((as->volume && as->volume->type == AFSVL_RWVOL) || rwpath)
			strcat(options, ",rwpath");
	}

	/* try and do the mount */
	_debug("--- attempting mount %s -o %s ---", devname, options);
	root = vfs_submount(mntpt, &afs_fs_type, devname, options);
	_debug("--- mount result %p ---", root);

	free_page((unsigned long) devname);
	free_page((unsigned long) options);
	_leave(" = %p", root);
	return root;

error:
	put_page(page);
error_no_page:
	free_page((unsigned long) options);
error_no_options:
	free_page((unsigned long) devname);
error_no_devname:
	_leave(" = %d", ret);
	return ERR_PTR(ret);
}
