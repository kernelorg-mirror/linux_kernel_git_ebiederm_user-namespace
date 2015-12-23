/* -*- linux-c -*- --------------------------------------------------------- *
 *
 * linux/fs/devpts/inode.c
 *
 *  Copyright 1998-2004 H. Peter Anvin -- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * ------------------------------------------------------------------------- */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/namei.h>
#include <linux/fs_struct.h>
#include <linux/slab.h>
#include <linux/mount.h>
#include <linux/tty.h>
#include <linux/mutex.h>
#include <linux/magic.h>
#include <linux/idr.h>
#include <linux/devpts_fs.h>
#include <linux/parser.h>
#include <linux/fsnotify.h>
#include <linux/seq_file.h>

#define DEVPTS_DEFAULT_MODE 0600

/*
 * sysctl support for setting limits on the number of Unix98 ptys allocated.
 * Otherwise one can eat up all kernel memory by opening /dev/ptmx repeatedly.
 */
static int pty_limit = NR_UNIX98_PTY_DEFAULT;
static int pty_reserve = NR_UNIX98_PTY_RESERVE;
static int pty_limit_min;
static int pty_limit_max = INT_MAX;
static int pty_count;

static struct ctl_table pty_table[] = {
	{
		.procname	= "max",
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.data		= &pty_limit,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &pty_limit_min,
		.extra2		= &pty_limit_max,
	}, {
		.procname	= "reserve",
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.data		= &pty_reserve,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &pty_limit_min,
		.extra2		= &pty_limit_max,
	}, {
		.procname	= "nr",
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.data		= &pty_count,
		.proc_handler	= proc_dointvec,
	},
	{}
};

static struct ctl_table pty_kern_table[] = {
	{
		.procname	= "pty",
		.mode		= 0555,
		.child		= pty_table,
	},
	{}
};

static struct ctl_table pty_root_table[] = {
	{
		.procname	= "kernel",
		.mode		= 0555,
		.child		= pty_kern_table,
	},
	{}
};

static DEFINE_MUTEX(allocated_ptys_lock);

static struct vfsmount *devpts_mnt;

struct pts_mount_opts {
	int setuid;
	int setgid;
	kuid_t   uid;
	kgid_t   gid;
	umode_t mode;
	umode_t ptmxmode;
	int max;
};

enum {
	Opt_uid, Opt_gid, Opt_mode, Opt_ptmxmode, Opt_newinstance,  Opt_max,
	Opt_err
};

static const match_table_t tokens = {
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_mode, "mode=%o"},
#ifdef CONFIG_DEVPTS_MULTIPLE_INSTANCES
	{Opt_ptmxmode, "ptmxmode=%o"},
	{Opt_newinstance, "newinstance"},
	{Opt_max, "max=%d"},
#endif
	{Opt_err, NULL}
};

struct pts_fs_info {
	struct ida allocated_ptys;
	struct pts_mount_opts mount_opts;
	struct dentry *ptmx_dentry;
};

static inline struct pts_fs_info *DEVPTS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct super_block *pts_sb_from_inode(struct inode *inode)
{
#ifdef CONFIG_DEVPTS_MULTIPLE_INSTANCES
	if (inode->i_sb->s_magic == DEVPTS_SUPER_MAGIC)
		return inode->i_sb;
#endif
	if (!devpts_mnt)
		return NULL;
	return devpts_mnt->mnt_sb;
}

static bool parse_newinstance(const char *data)
{
	while (data) {
		const char *p = strchr(data, ',');
		size_t len = p ? p - data : strlen(data);
		if ((len == 11) && (memcmp(data, "newinstance", 11) == 0)) {
			return true;
		}
		data = p ? p + 1 : NULL;
	}
	return false;
}

/*
 * parse_mount_options():
 *	Set @opts to mount options specified in @data. If an option is not
 *	specified in @data, set it to its default value.
 *
 * Note: @data may be NULL (in which case all options are set to default).
 */
static int parse_mount_options(char *data, struct pts_mount_opts *opts)
{
	char *p;
	kuid_t uid;
	kgid_t gid;

	opts->setuid  = 0;
	opts->setgid  = 0;
	opts->uid     = GLOBAL_ROOT_UID;
	opts->gid     = GLOBAL_ROOT_GID;
	opts->mode    = DEVPTS_DEFAULT_MODE;
	opts->ptmxmode = DEVPTS_DEFAULT_PTMX_MODE;
	opts->max     = NR_UNIX98_PTY_MAX;

	while ((p = strsep(&data, ",")) != NULL) {
		substring_t args[MAX_OPT_ARGS];
		int token;
		int option;

		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_uid:
			if (match_int(&args[0], &option))
				return -EINVAL;
			uid = make_kuid(current_user_ns(), option);
			if (!uid_valid(uid))
				return -EINVAL;
			opts->uid = uid;
			opts->setuid = 1;
			break;
		case Opt_gid:
			if (match_int(&args[0], &option))
				return -EINVAL;
			gid = make_kgid(current_user_ns(), option);
			if (!gid_valid(gid))
				return -EINVAL;
			opts->gid = gid;
			opts->setgid = 1;
			break;
		case Opt_mode:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->mode = option & S_IALLUGO;
			break;
#ifdef CONFIG_DEVPTS_MULTIPLE_INSTANCES
		case Opt_ptmxmode:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->ptmxmode = option & S_IALLUGO;
			break;
		case Opt_newinstance:
			break;
		case Opt_max:
			if (match_int(&args[0], &option) ||
			    option < 0 || option > NR_UNIX98_PTY_MAX)
				return -EINVAL;
			opts->max = option;
			break;
#endif
		default:
			pr_err("called with bogus options\n");
			return -EINVAL;
		}
	}

	return 0;
}

#ifdef CONFIG_DEVPTS_MULTIPLE_INSTANCES
static int mknod_ptmx(struct super_block *sb)
{
	int rc = -ENOMEM;
	struct dentry *dentry;
	struct inode *inode;
	struct dentry *root = sb->s_root;
	struct pts_fs_info *fsi = DEVPTS_SB(sb);
	struct pts_mount_opts *opts = &fsi->mount_opts;
	kuid_t root_uid;
	kgid_t root_gid;

	root_uid = make_kuid(current_user_ns(), 0);
	root_gid = make_kgid(current_user_ns(), 0);
	if (!uid_valid(root_uid) || !gid_valid(root_gid))
		return -EINVAL;

	inode_lock(d_inode(root));

	dentry = d_alloc_name(root, "ptmx");
	if (!dentry) {
		pr_err("Unable to alloc dentry for ptmx node\n");
		goto out;
	}

	/*
	 * Create a new 'ptmx' node in this mount of devpts.
	 */
	inode = new_inode(sb);
	if (!inode) {
		pr_err("Unable to alloc inode for ptmx node\n");
		dput(dentry);
		goto out;
	}

	inode->i_ino = 2;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;

	inode->i_mode = S_IFCHR|opts->ptmxmode;
	inode->i_fop = &ptmx_fops;
	inode->i_rdev = MKDEV(TTYAUX_MAJOR, PTMX_MINOR);
	inode->i_uid = root_uid;
	inode->i_gid = root_gid;

	d_add(dentry, inode);

	fsi->ptmx_dentry = dentry;
	rc = 0;
out:
	inode_unlock(d_inode(root));
	return rc;
}

static void update_ptmx_mode(struct pts_fs_info *fsi)
{
	struct inode *inode;
	if (fsi->ptmx_dentry) {
		inode = d_inode(fsi->ptmx_dentry);
		inode->i_mode = S_IFCHR|fsi->mount_opts.ptmxmode;
	}
}
#else
static inline void update_ptmx_mode(struct pts_fs_info *fsi)
{
	return;
}
static inline int mknod_ptmx(struct super_block *sb)
{
	return 0;
}
#endif

static int devpts_remount(struct super_block *sb, int *flags, char *data)
{
	int err;
	struct pts_fs_info *fsi = DEVPTS_SB(sb);
	struct pts_mount_opts *opts = &fsi->mount_opts;

	sync_filesystem(sb);
	err = parse_mount_options(data, opts);

	/*
	 * parse_mount_options() restores options to default values
	 * before parsing and may have changed ptmxmode. So, update the
	 * mode in the inode too. Bogus options don't fail the remount,
	 * so do this even on error return.
	 */
	update_ptmx_mode(fsi);

	return err;
}

static int devpts_show_options(struct seq_file *seq, struct dentry *root)
{
	struct pts_fs_info *fsi = DEVPTS_SB(root->d_sb);
	struct pts_mount_opts *opts = &fsi->mount_opts;

	if (opts->setuid)
		seq_printf(seq, ",uid=%u",
			   from_kuid_munged(&init_user_ns, opts->uid));
	if (opts->setgid)
		seq_printf(seq, ",gid=%u",
			   from_kgid_munged(&init_user_ns, opts->gid));
	seq_printf(seq, ",mode=%03o", opts->mode);
#ifdef CONFIG_DEVPTS_MULTIPLE_INSTANCES
	seq_printf(seq, ",ptmxmode=%03o", opts->ptmxmode);
	if (opts->max < NR_UNIX98_PTY_MAX)
		seq_printf(seq, ",max=%d", opts->max);
#endif

	return 0;
}

bool devpts_may_overmount(struct super_block *sb,
			  int flags, const char *dev_name, void *data)
{
	if ((sb == devpts_mnt->mnt_sb) &&
	    (current_user_ns() == &init_user_ns) &&
	    !parse_newinstance(data)) {
		down_write(&sb->s_umount);
		devpts_remount(sb, &flags, data);
		up_write(&sb->s_umount);
		return false;
	}
	return true;
}

static const struct super_operations devpts_sops = {
	.statfs		= simple_statfs,
	.remount_fs	= devpts_remount,
	.show_options	= devpts_show_options,
	.may_overmount	= devpts_may_overmount,
};

static void *new_pts_fs_info(void)
{
	struct pts_fs_info *fsi;

	fsi = kzalloc(sizeof(struct pts_fs_info), GFP_KERNEL);
	if (!fsi)
		return NULL;

	ida_init(&fsi->allocated_ptys);
	fsi->mount_opts.mode = DEVPTS_DEFAULT_MODE;
	fsi->mount_opts.ptmxmode = DEVPTS_DEFAULT_PTMX_MODE;

	return fsi;
}

static int
devpts_fill_super(struct super_block *s, void *data, int silent)
{
	struct inode *inode;
	int error;

	s->s_blocksize = 1024;
	s->s_blocksize_bits = 10;
	s->s_magic = DEVPTS_SUPER_MAGIC;
	s->s_op = &devpts_sops;
	s->s_time_gran = 1;

	error = -ENOMEM;
	s->s_fs_info = new_pts_fs_info();
	if (!s->s_fs_info)
		goto fail;

	error = parse_mount_options(data, &DEVPTS_SB(s)->mount_opts);
	if (error)
		goto fail;

	error = -ENOMEM;
	inode = new_inode(s);
	if (!inode)
		goto fail;
	inode->i_ino = 1;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_mode = S_IFDIR | S_IRUGO | S_IXUGO | S_IWUSR;
	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	set_nlink(inode, 2);

	s->s_root = d_make_root(inode);
	if (!s->s_root) {
		pr_err("get root dentry failed\n");
		goto fail;
	}

	error = mknod_ptmx(s);
	if (error)
		goto fail_dput;

	return 0;
fail_dput:
	dput(s->s_root);
	s->s_root = NULL;
fail:
	return error;
}

#ifdef CONFIG_DEVPTS_MULTIPLE_INSTANCES
/*
 * devpts_mount()
 *
 *     If the '-o newinstance' mount option was specified, mount a new
 *     (private) instance of devpts.  PTYs created in this instance are
 *     independent of the PTYs in other devpts instances.
 *
 *     If the '-o newinstance' option was not specified, mount/remount the
 *     initial kernel mount of devpts.  This type of mount gives the
 *     legacy, single-instance semantics.
 *
 *     The 'newinstance' option is needed to support multiple namespace
 *     semantics in devpts while preserving backward compatibility of the
 *     current 'single-namespace' semantics. i.e all mounts of devpts
 *     without the 'newinstance' mount option should bind to the initial
 *     kernel mount, like mount_single().
 *
 *     Mounts with 'newinstance' option create a new, private namespace.
 *
 *     NOTE:
 *
 *     For single-mount semantics, devpts cannot use mount_single(),
 *     because mount_single()/sget() find and use the super-block from
 *     the most recent mount of devpts. But that recent mount may be a
 *     'newinstance' mount and mount_single() would pick the newinstance
 *     super-block instead of the initial super-block.
 */
static struct dentry *devpts_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	struct dentry *root;
	bool newinstance;

	newinstance = parse_newinstance(data);
	if (flags & MS_KERNMOUNT)
		newinstance = true;

	/* Force newinstance for all user namespace mounts to ensure
	 * the mount options are not changed.
	 */
	if (current_user_ns() != &init_user_ns)
		newinstance = true;

	root = NULL;
	if (!newinstance)
		root = mount_super_once(devpts_mnt->mnt_sb, flags, data);
	if (IS_ERR_OR_NULL(root))
		root = mount_nodev(fs_type, flags, data, devpts_fill_super);

	return root;
}

#else
/*
 * This supports only the legacy single-instance semantics (no
 * multiple-instance semantics)
 */
static struct dentry *devpts_mount(struct file_system_type *fs_type, int flags,
		const char *dev_name, void *data)
{
	return mount_single(fs_type, flags, data, devpts_fill_super);
}
#endif

static void devpts_kill_sb(struct super_block *sb)
{
	struct pts_fs_info *fsi = DEVPTS_SB(sb);

	if (fsi)
		ida_destroy(&fsi->allocated_ptys);
	kfree(fsi);
	kill_litter_super(sb);
}

static struct file_system_type devpts_fs_type = {
	.name		= "devpts",
	.mount		= devpts_mount,
	.kill_sb	= devpts_kill_sb,
#ifdef CONFIG_DEVPTS_MULTIPLE_INSTANCES
	.fs_flags	= FS_USERNS_MOUNT | FS_USERNS_DEV_MOUNT,
#endif
};

/*
 * The normal naming convention is simply /dev/pts/<number>; this conforms
 * to the System V naming convention
 */

int devpts_new_index(struct inode *ptmx_inode)
{
	struct super_block *sb = pts_sb_from_inode(ptmx_inode);
	struct pts_fs_info *fsi;
	int index;
	int ida_ret;

	if (!sb)
		return -ENODEV;

	fsi = DEVPTS_SB(sb);
retry:
	if (!ida_pre_get(&fsi->allocated_ptys, GFP_KERNEL))
		return -ENOMEM;

	mutex_lock(&allocated_ptys_lock);
	if (pty_count >= pty_limit -
			((devpts_mnt->mnt_sb == sb) ? pty_reserve : 0)) {
		mutex_unlock(&allocated_ptys_lock);
		return -ENOSPC;
	}

	ida_ret = ida_get_new(&fsi->allocated_ptys, &index);
	if (ida_ret < 0) {
		mutex_unlock(&allocated_ptys_lock);
		if (ida_ret == -EAGAIN)
			goto retry;
		return -EIO;
	}

	if (index >= fsi->mount_opts.max) {
		ida_remove(&fsi->allocated_ptys, index);
		mutex_unlock(&allocated_ptys_lock);
		return -ENOSPC;
	}
	pty_count++;
	mutex_unlock(&allocated_ptys_lock);
	return index;
}

void devpts_kill_index(struct inode *ptmx_inode, int idx)
{
	struct super_block *sb = pts_sb_from_inode(ptmx_inode);
	struct pts_fs_info *fsi = DEVPTS_SB(sb);

	mutex_lock(&allocated_ptys_lock);
	ida_remove(&fsi->allocated_ptys, idx);
	pty_count--;
	mutex_unlock(&allocated_ptys_lock);
}

/*
 * pty code needs to hold extra references in case of last /dev/tty close
 */

void devpts_add_ref(struct inode *ptmx_inode)
{
	struct super_block *sb = pts_sb_from_inode(ptmx_inode);

	atomic_inc(&sb->s_active);
	ihold(ptmx_inode);
}

void devpts_del_ref(struct inode *ptmx_inode)
{
	struct super_block *sb = pts_sb_from_inode(ptmx_inode);

	iput(ptmx_inode);
	deactivate_super(sb);
}

/**
 * devpts_pty_new -- create a new inode in /dev/pts/
 * @ptmx_inode: inode of the master
 * @device: major+minor of the node to be created
 * @index: used as a name of the node
 * @priv: what's given back by devpts_get_priv
 *
 * The created inode is returned. Remove it from /dev/pts/ by devpts_pty_kill.
 */
struct inode *devpts_pty_new(struct inode *ptmx_inode, dev_t device, int index,
		void *priv)
{
	struct dentry *dentry;
	struct super_block *sb = pts_sb_from_inode(ptmx_inode);
	struct inode *inode;
	struct dentry *root;
	struct pts_fs_info *fsi;
	struct pts_mount_opts *opts;
	char s[12];

	if (!sb)
		return ERR_PTR(-ENODEV);

	root = sb->s_root;
	fsi = DEVPTS_SB(sb);
	opts = &fsi->mount_opts;

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	inode->i_ino = index + 3;
	inode->i_uid = opts->setuid ? opts->uid : current_fsuid();
	inode->i_gid = opts->setgid ? opts->gid : current_fsgid();
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	init_special_inode(inode, S_IFCHR|opts->mode, device);
	inode->i_private = priv;

	sprintf(s, "%d", index);

	inode_lock(d_inode(root));

	dentry = d_alloc_name(root, s);
	if (dentry) {
		d_add(dentry, inode);
		fsnotify_create(d_inode(root), dentry);
	} else {
		iput(inode);
		inode = ERR_PTR(-ENOMEM);
	}

	inode_unlock(d_inode(root));

	return inode;
}

/**
 * devpts_get_priv -- get private data for a slave
 * @pts_inode: inode of the slave
 *
 * Returns whatever was passed as priv in devpts_pty_new for a given inode.
 */
void *devpts_get_priv(struct inode *pts_inode)
{
	struct dentry *dentry;
	void *priv = NULL;

	BUG_ON(pts_inode->i_rdev == MKDEV(TTYAUX_MAJOR, PTMX_MINOR));

	/* Ensure dentry has not been deleted by devpts_pty_kill() */
	dentry = d_find_alias(pts_inode);
	if (!dentry)
		return NULL;

	if (pts_inode->i_sb->s_magic == DEVPTS_SUPER_MAGIC)
		priv = pts_inode->i_private;

	dput(dentry);

	return priv;
}

/**
 * devpts_pty_kill -- remove inode form /dev/pts/
 * @inode: inode of the slave to be removed
 *
 * This is an inverse operation of devpts_pty_new.
 */
void devpts_pty_kill(struct inode *inode)
{
	struct super_block *sb = pts_sb_from_inode(inode);
	struct dentry *root = sb->s_root;
	struct dentry *dentry;

	BUG_ON(inode->i_rdev == MKDEV(TTYAUX_MAJOR, PTMX_MINOR));

	inode_lock(d_inode(root));

	dentry = d_find_alias(inode);

	drop_nlink(inode);
	d_delete(dentry);
	dput(dentry);	/* d_alloc_name() in devpts_pty_new() */
	dput(dentry);		/* d_find_alias above */

	inode_unlock(d_inode(root));
}

static void ptmx_expire_automounts(struct work_struct *work);
static LIST_HEAD(ptmx_automounts);
static DECLARE_DELAYED_WORK(ptmx_automount_work, ptmx_expire_automounts);
static unsigned long ptmx_automount_timeout = 10 * 60 * HZ;

static void ptmx_expire_automounts(struct work_struct *work)
{
	struct list_head *list = &ptmx_automounts;

	mark_mounts_for_expiry(list);
	if (!list_empty(list))
		schedule_delayed_work(&ptmx_automount_work,
				      ptmx_automount_timeout);
}

struct vfsmount *ptmx_automount(struct path *input_path)
{
	struct vfsmount *newmnt;
	struct path path;
	struct dentry *old;

	/* Can the pts filesystem be found with a path walk? */
	path = *input_path;
	path_get(&path);

	if ((path_pts(&path) != 0) ||
	    /* Is the path the root of a devpts filesystem? */
	    (path.mnt->mnt_sb->s_magic != DEVPTS_SUPER_MAGIC) ||
	    (path.mnt->mnt_root != path.mnt->mnt_sb->s_root)) {
		/* No devpts filesystem found use the system devpts */
		path_put(&path);
		path.mnt = devpts_mnt;
		path.dentry = DEVPTS_SB(devpts_mnt->mnt_sb)->ptmx_dentry;
		path_get(&path);
	}
	else {
		/* Advance path to the ptmx dentry */
		old = path.dentry;
		path.dentry = dget(DEVPTS_SB(path.mnt->mnt_sb)->ptmx_dentry);
		dput(old);
	}

	newmnt = vfs_loopback_mount(&path);
	if (IS_ERR(newmnt))
		goto fail;

	mntget(newmnt);
	mnt_set_expiry(newmnt, &ptmx_automounts);
	schedule_delayed_work(&ptmx_automount_work, ptmx_automount_timeout);
fail:
	path_put(&path);
	return newmnt;
}

static int __init init_devpts_fs(void)
{
	int err = register_filesystem(&devpts_fs_type);
	struct ctl_table_header *table;

	if (!err) {
		struct vfsmount *mnt;

		table = register_sysctl_table(pty_root_table);
		mnt = kern_mount(&devpts_fs_type);
		if (IS_ERR(mnt)) {
			err = PTR_ERR(mnt);
			unregister_filesystem(&devpts_fs_type);
			unregister_sysctl_table(table);
		} else {
			devpts_mnt = mnt;
		}
	}
	return err;
}
module_init(init_devpts_fs)
