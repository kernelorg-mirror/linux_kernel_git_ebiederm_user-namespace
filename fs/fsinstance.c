#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>
#include <linux/security.h>
#include <linux/anon_inodes.h>
#include <linux/namei.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/parser.h>
#include <linux/fsoptions.h>
#include <linux/mount.h>
#include "mount.h"
#include "internal.h"

struct fs_driver {
	struct file_system_type *type;
	const char *type_name;
	const char *subtype;
};

static int fs_driver_release(struct inode *inode, struct file *file)
{
	struct fs_driver *driver = file->private_data;
	kfree(driver->type_name);
	put_filesystem(driver->type);
	kfree(driver);
	return 0;
}

const struct file_operations fs_driver_fops = {
	.release	= fs_driver_release,
	.llseek		= no_llseek,
};

/*
 * Open a filesystem driver by name so that it can be configured for mounting.
 *
 */
SYSCALL_DEFINE1(fsopen,	const char __user *, _type_name)
{
	struct file_system_type *type = NULL;
	struct fs_driver *fs_driver = NULL;
	const char *type_name;
	const char *subtype;
	int ret;

	type_name = strndup_user(_type_name, PAGE_SIZE);
	if (IS_ERR(type_name))
		return PTR_ERR(type_name);

	type = get_fs_type(type_name);
	ret = PTR_ERR(type);
	if (IS_ERR(type))
		goto out;
	subtype = fs_subtype(type_name);
	ret = PTR_ERR(subtype);
	if (IS_ERR(subtype))
		goto out;

	ret = 0;
	if (type->permission)
		ret = type->permission();
	else if (!capable(CAP_SYS_ADMIN))
		ret = -EPERM;
	if (ret)
		goto out;

	ret = -ENOMEM;
	fs_driver = kzalloc(sizeof(struct fs_driver), GFP_KERNEL);
	if (!fs_driver)
		goto out;

	fs_driver->type = type;
	fs_driver->type_name = type_name;
	fs_driver->subtype = subtype;
	ret = anon_inode_getfd("fs_driver", &fs_driver_fops, fs_driver, O_RDWR | O_CLOEXEC);
	if (ret >= 0) {
		type_name = NULL;
		type = NULL;
		fs_driver = NULL;
	}
out:
	kfree(fs_driver);
	if (type && !IS_ERR(type))
		put_filesystem(type);
	kfree(type_name);
	return ret;
}


enum {
	Opt_ro,
	Opt_rw,
	Opt_nosuid,
	Opt_suid,
	Opt_nodev,
	Opt_dev,
	Opt_noexec,
	Opt_exec,
	Opt_sync,
	Opt_async,
	Opt_mandlock,
	Opt_advisorylock,
	Opt_dirsync,
	Opt_dirasync,
	Opt_noatime,
	/* Opt_atime, */
	Opt_nodiratime,
	Opt_diratime,
	Opt_silent,
	Opt_loud,
	Opt_relatime,
	/* Opt_norelatime, */
	Opt_iversion,
	Opt_noiversion,
	Opt_strictatime,
	Opt_nostrictatime,
	Opt_lazytime,
	Opt_nolazytime,
};
#define Opt_count (Opt_nolazytime + 1)

static const struct match_table vfs_tokens[] = {
	{ Opt_ro, "ro" },
	{ Opt_rw, "rw" },
	{ Opt_nosuid, "nosuid" },
	{ Opt_suid, "suid" },
	{ Opt_nodev, "nodev" },
	{ Opt_dev, "dev" },
	{ Opt_noexec,  "noexec" },
	{ Opt_exec, "exec" },
	{ Opt_sync, "sync" },
	{ Opt_async, "async" },
	/* MS_REMOUNT "remount" */
	{ Opt_mandlock, "mandlock" },
	{ Opt_mandlock, "mand" },
	{ Opt_advisorylock, "nomand" },
	{ Opt_dirsync, "dirsync" },
	{ Opt_dirasync, "dirasync" }, /* Not implemented by mount(8) */
	{ Opt_noatime, "noatime" },
	/* { Opt_atime, "atime" }, */

	/* MS_BIND "bind" */
	/* MS_REC|MS_BIND "rbind" */
	/* MS_MOVE */
	/* MS_REC */

	{ Opt_nodiratime, "nodiratime" },
	{ Opt_diratime, "diratime" },
	{ Opt_silent, "silent" },
	{ Opt_loud, "loud" },

	/* MS_POSIXACL can not be set??? */
	/* Some filesystems supports acl and noacl for setting POSIXACL */

	/* MS_UNBINDABLE "unbindable" */
	/* MS_REC|MS_UNBINDABLE "runbindable" */
	/* MS_PRIVATE "private" */
	/* MS_REC|MS_PRIVATE "rprivate" */
	/* MS_SLAVE "slave" */
	/* MS_REC|MS_SLAVE "rslave" */
	/* MS_SHARED "shared" */
	/* MS_REC|MS_SHARED "rshared" */

	{ Opt_relatime, "relatime" },
	/* { Opt_norelatime, "norelatime" }, */

	/* MS_KERNMOUNT -- internal only */

	{ Opt_iversion, "iversion" },
	{ Opt_noiversion, "noiversion" },
	{ Opt_strictatime, "strictatime" },
	{ Opt_nostrictatime, "nostrictatime" },
	{ Opt_lazytime, "lazytime" },
	{ Opt_nolazytime, "nolazytime" },

	/* MS_SUBMOUNT  -- internal only */
	/* MS_NOREMOTELOCK -- internal only */
	/* MS_NOSEC -- internal only */
	/* MS_BORN -- internal only */
	/* MS_ACTIVE -- internal only */
	/* MS_NOUSER -- internal only */
	{ }
};

static const struct {
	unsigned int flag;
	bool set;
} sb_token[Opt_count] = {
	[Opt_ro]           = { .flag = SB_RDONLY,      .set = true },
	[Opt_rw]           = { .flag = SB_RDONLY,      .set = false },
	[Opt_sync]         = { .flag = SB_SYNCHRONOUS, .set = true },
	[Opt_async]        = { .flag = SB_SYNCHRONOUS, .set = false },
	[Opt_mandlock]     = { .flag = SB_MANDLOCK,    .set = true },
	[Opt_advisorylock] = { .flag = SB_MANDLOCK,    .set = false },
	[Opt_dirsync]      = { .flag = SB_DIRSYNC,     .set = true },
	[Opt_dirasync]     = { .flag = SB_DIRSYNC,     .set = false },
	[Opt_silent]       = { .flag = SB_SILENT,      .set = true },
	[Opt_loud]         = { .flag = SB_SILENT,      .set = false },
	/* posixacl? */
	[Opt_lazytime]     = { .flag = SB_LAZYTIME,    .set = true },
	[Opt_nolazytime]   = { .flag = SB_LAZYTIME,    .set = false },
	[Opt_iversion]     = { .flag = SB_I_VERSION,   .set = true },
	[Opt_noiversion]   = { .flag = SB_I_VERSION,   .set = false },
};

SYSCALL_DEFINE3(fsspecifiers,
		int, driverfd,
		char __user *, specifiers,
		size_t, specifiers_len)
{
	const struct match_table *token;
	struct file_system_type *type;
	struct fs_driver *driver;
	char *buf = NULL;
	struct fd f;
	ssize_t ret;
	size_t bufsz, off;

	ret = -EBADF;
	f = fdget(driverfd);
	if (!f.file)
		goto out;
	if (f.file->f_op != &fs_driver_fops)
		goto out;
	driver = f.file->private_data;
	type = driver->type;

	ret = -ENOMEM;
	bufsz = PAGE_SIZE;
	buf = kzalloc(bufsz, GFP_KERNEL);
	if (!buf)
		goto out;

	off = 0;
	for (token = vfs_tokens; token->pattern; token++) {
		unsigned int flag = sb_token[token->token].flag;
		if (!flag || !(flag & (SB_RDONLY | type->sb_flags_mask)))
			continue;
		if (off != 0)
			off += scnprintf(buf + off, bufsz - off, ",");
		off += scnprintf(buf + off, bufsz - off, "%s", token->pattern);
	}
	token = type->open_tokens;
	if (token) {
		for (; token->pattern; token++) {
			const char *sep = strchr(token->pattern, '=');
			const char *format = strchr(token->pattern, '%');
			int len;

			len = sep ? sep - token->pattern : strlen(token->pattern);

			/* Ensure '%' does not appear in an option */
			if (format && ((format - token->pattern) <= len)) {
				WARN_ON_ONCE(true);
				continue;
			}

			off += scnprintf(buf + off, bufsz - off, ",%*.*s",
					 len, len, token->pattern);
		}
	}
	off += 1;  /* Include the terminating NULL */

	ret = -ERANGE;
	if (off > specifiers_len)
		goto out;

	ret = -EFAULT;
	if (copy_to_user(specifiers, buf, off))
		goto out;

	ret = off;
out:
	kfree(buf);
	fdput(f);
	return ret;
}

struct fs_instance {
	struct super_block *sb;
	struct dentry *root;
	void *state;
	const char *name;
	unsigned long configure_seq;
	unsigned int sb_omask;
	unsigned int sb_oflags;
};

static void fs_instance_free(struct fs_instance *instance)
{
	struct super_block *sb = instance->sb;
	void *state = instance->state;

	kfree(instance->name);
	if (sb && state && sb->s_op->release_state)
		sb->s_op->release_state(state);
	dput(instance->root);
	if (sb)
		deactivate_super(sb);
	kfree(instance);
}

static int fs_instance_release(struct inode *inode, struct file *file)
{
	fs_instance_free(file->private_data);
	return 0;
}

const struct file_operations fs_instance_fops = {
	.release	= fs_instance_release,
	.llseek		= no_llseek,
};

static int parse_sb_flags(unsigned int amask, unsigned int *sb_flagsp,
			  const char **vfsv)
{
	unsigned int sb_flags = *sb_flagsp;
	const char **opt;


	for (opt = vfsv; *opt; opt++) {
		substring_t args[MAX_OPT_ARGS];
		int token;
		unsigned int flag;

		token = match_token(*opt, vfs_tokens, args);

		/* A valid option? */
		if ((token == MATCH_FAILURE) || (token >= Opt_count))
			return -EINVAL;

		/* Is the vfs option accepted? */
		flag = sb_token[token].flag;
		if (!flag || ((flag & amask) == 0))
			return -EINVAL;

		if (sb_token[token].set)
			sb_flags |= flag;
		else
			sb_flags &= ~flag;
	}
	*sb_flagsp = sb_flags;
	return 0;
}

/*
 * Open a filesystem by name so that it can be configured for mounting.
 *
 */
SYSCALL_DEFINE3(fsinstance,
		int, driverfd,
		const char __user *, _fs_name,
		const char __user *, _fs_specifiers)
{
	const char **vfsv = NULL, **fsv = NULL;
	struct fs_instance *instance = NULL;
	struct file_system_type *type;
	const char *fs_name = NULL;
	char *fs_specifiers = NULL;
	struct fs_driver *driver;
	struct super_block *sb;
	unsigned int sb_omask;
	unsigned int sb_oflags;
	const char *subtype;
	struct fd f;
	int ret;

	ret = -EBADF;
	f = fdget(driverfd);
	if (!f.file)
		goto out;
	if (f.file->f_op != &fs_driver_fops)
		goto out;
	driver = f.file->private_data;
	type = driver->type;
	subtype = driver->subtype;

	ret = -ENOMEM;
	instance = kzalloc(sizeof(struct fs_instance), GFP_KERNEL);
	if (!instance)
		goto out;

	fs_name = strndup_user(_fs_name, PAGE_SIZE);
	if (IS_ERR(fs_name)) {
		ret = PTR_ERR(fs_name);
		fs_name = NULL;
		goto out;
	}

	fs_specifiers = strndup_user(_fs_specifiers, PAGE_SIZE);
	if (IS_ERR(fs_specifiers)) {
		ret = PTR_ERR(fs_specifiers);
		fs_specifiers = NULL;
		goto out;
	}

	ret = -EOPNOTSUPP;
	if (!type->open)
		goto out;

	ret = split_options(fs_specifiers, vfs_tokens, NULL,
			    &vfsv, NULL, &fsv);
	if (ret)
		goto out;

	sb_oflags = 0;
	sb_omask = SB_RDONLY | type->sb_flags_mask;
	ret = parse_sb_flags(sb_omask, &sb_oflags, vfsv);
	if (ret)
		goto out;

	sb = type->open(type, sb_oflags, current_user_ns(),
			fs_name, fsv,  &instance->state);
	ret = PTR_ERR(sb);
	if (IS_ERR(sb))
		goto out;

	if (sb->s_subtype) {
		ret = -EBUSY;
		if (!subtype || (strcmp(sb->s_subtype, subtype) != 0))
			goto out_sb;
	} else if (subtype) {
		sb->s_subtype = kstrdup(subtype, GFP_KERNEL);
		ret = -ENOMEM;
		if (!sb->s_subtype)
			goto out_sb;
	}

	instance->root = NULL;
	instance->sb = sb;
	instance->sb_omask = sb_omask;
	instance->sb_oflags = sb_oflags;
	instance->configure_seq = 1;
	instance->name = fs_name;
	fs_name = NULL;
	up_write(&sb->s_umount);
	ret = anon_inode_getfd("fs_instance", &fs_instance_fops, instance,
			       O_RDWR | O_CLOEXEC);
out:
	kfree(vfsv);
	kfree(fsv);
	if (ret < 0)
		fs_instance_free(instance);
	kfree(fs_name);
	kfree(fs_specifiers);
	fdput(f);
	return ret;
out_sb:
	deactivate_locked_super(sb);
	goto out;
}

SYSCALL_DEFINE3(fspick,
		int, dfd,
		const char __user *, path,
		unsigned int, flags)
{
	unsigned int lookup_flags = LOOKUP_FOLLOW | LOOKUP_AUTOMOUNT;
	struct fs_instance *instance = NULL;
	struct path target = { .mnt = NULL, .dentry = NULL };
	struct super_block *sb;
	struct vfsmount *mnt;
	struct dentry *root;
	int ret;

	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH))
		return -EINVAL;

	if (flags & AT_SYMLINK_NOFOLLOW)
		lookup_flags &= ~LOOKUP_FOLLOW;
	if (flags & AT_NO_AUTOMOUNT)
		lookup_flags &= ~LOOKUP_AUTOMOUNT;
	if (flags & AT_EMPTY_PATH)
		lookup_flags |= LOOKUP_EMPTY;

	ret = user_path_at(dfd, path, 0, &target);
	if (ret)
		return ret;

	root = target.dentry;
	mnt = target.mnt;
	sb = mnt->mnt_sb;

	ret = -ENOTDIR;
	if (!d_is_dir(root))
		goto out;

	ret = -EINVAL;
	if (sb->s_flags & SB_NOUSER)
		goto out;

	ret = -EPERM;
	if (!ns_capable(sb->s_user_ns, CAP_SYS_ADMIN))
		goto out;

	ret = -ENOMEM;
	instance = kzalloc(sizeof(struct fs_instance), GFP_KERNEL);
	if (!instance)
		goto out;

	ret = -ENOMEM;
	instance->name = kstrdup(real_mount(mnt)->mnt_devname, GFP_KERNEL);
	if (!instance->name)
		goto out;

	atomic_inc(&sb->s_active);
	instance->sb = sb;
	instance->root = dget(root);
	instance->configure_seq = 0; /* Call fsoptions to set */

	ret = anon_inode_getfd("fs_instance", &fs_instance_fops, instance,
			       O_RDWR | O_CLOEXEC);
out:
	if ((ret < 0) && instance)
		fs_instance_free(instance);
	path_put(&target);
	return ret;
}

SYSCALL_DEFINE2(fsset,
		int, instancefd,
		const char __user *, _options)
{
	const char **secv = NULL, **vfsv = NULL, **fsv = NULL;
	unsigned int sb_flags, sb_amask;
	struct fs_instance *instance;
	unsigned long configure_seq;
	struct super_block *sb;
	char *options = NULL;
	struct fd f;
	int ret;

	ret = -EBADF;
	f = fdget(instancefd);
	if (!f.file)
		goto out;
	if (f.file->f_op != &fs_instance_fops)
		goto out;
	instance = f.file->private_data;
	sb = instance->sb;

	ret = -EOPNOTSUPP;
	if (!sb->s_op->init)
		goto out;

	options = strndup_user(_options, PAGE_SIZE);
	ret = PTR_ERR(options);
	if (IS_ERR(options))
		goto out;

	ret = split_options(options, security_tokens, vfs_tokens,
			    &secv, &vfsv, &fsv);
	if (ret)
		goto out;

	down_write(&sb->s_umount);
	ret = -ESTALE;
	configure_seq = sb->s_configure_seq;
	if (configure_seq != instance->configure_seq)
		goto out_unlock;
	/* Don't try and handle a non-sense state */
	if (!sb->s_root && instance->root)
		goto out_unlock;

	/* Start with the existing super block flags */
	sb_amask = SB_OPT_FLAGS;
	sb_flags = 0;
	if (configure_seq == 1) {
		sb_amask &= ~instance->sb_omask;
		sb_flags |= instance->sb_oflags;
	}
	sb_flags |= sb->s_flags & sb_amask;
	ret = parse_sb_flags(sb_amask, &sb_flags, vfsv);
	if (ret)
		goto out_unlock;

	if (configure_seq == 1) {
		ret = -EEXIST;
		if (sb->s_flags & SB_ACTIVE)
			goto out_unlock;

		sb->s_configure_seq++;
		sb->s_flags = (sb->s_flags & ~SB_OPT_FLAGS) | sb_flags;
		ret = sb->s_op->init(sb, instance->state, fsv);
		if (ret)
			goto out_sb;

		ret = security_sb_set_mnt_opts(sb, secv);
		if (ret)
			goto out_sb;

		finish_super(sb);
	} else {
		ret = security_sb_remount(sb, secv);
		if (ret)
			goto out_sb;

		ret = do_remount_sb(sb, sb_flags, fsv, 0);
		if (ret)
			goto out_sb;

		up_write(&sb->s_umount);
	}

	if (!instance->root) {
		struct dentry *root;

		if (sb->s_op->root) {
			root = sb->s_op->root(sb, instance->state, fsv);
			ret = PTR_ERR(root);
			if (IS_ERR(root))
				goto out_unlocked_sb;
			sb = root->d_sb;
		} else {
			root = dget(sb->s_root);
		}
		instance->sb = sb;
		instance->root = root;
		instance->state = NULL;
	}
	ret = 0;

out:
	kfree(secv);
	kfree(vfsv);
	kfree(fsv);
	kfree(options);
	fdput(f);
	return ret;
out_unlock:
	up_write(&sb->s_umount);
	goto out;
out_unlocked_sb:
	down_write(&sb->s_umount);
out_sb:
	if (instance->state && sb->s_op->release_state)
		sb->s_op->release_state(instance->state);
	instance->state = NULL;

	dput(instance->root);
	instance->root = NULL;
	deactivate_locked_super(sb);
	instance->sb = NULL;
	goto out;
}

SYSCALL_DEFINE2(fsmount,
		int, instancefd,
		const char __user *, _options)
{
	const char **opt, **vfsv = NULL, **fsv = NULL;
	struct fs_instance *instance = NULL;
	unsigned int mnt_flags = 0;
	char *options = NULL;
	struct dentry *root;
	struct file *file;
	struct path path = { .mnt = NULL, .dentry = NULL };
	struct fd f;
	int ret;

	ret = -EBADF;
	f = fdget(instancefd);
	if (!f.file)
		goto out;
	if (f.file->f_op != &fs_instance_fops)
		goto out;
	instance = f.file->private_data;
	if (!instance->root)
		goto out;
	if (instance->state)
		goto out;

	options = strndup_user(_options, PAGE_SIZE);
	ret = PTR_ERR(options);
	if (IS_ERR(options))
		goto out;

	ret = split_options(options, vfs_tokens, NULL,
			    &vfsv, NULL, &fsv);
	if (ret)
		goto out;

	ret = -EINVAL;
	if (fsv[0] != NULL)
		goto out;

	/* Default to relatime unless overridden */
	mnt_flags |= MNT_RELATIME;
	for (opt = vfsv; *opt; opt++) {
		substring_t args[MAX_OPT_ARGS];
		int token;

		token = match_token(*opt, vfs_tokens, args);
		switch (token) {
		case Opt_ro:		mnt_flags |= MNT_READONLY; break;
		case Opt_rw:		mnt_flags &= ~MNT_READONLY; break;
		case Opt_nosuid:	mnt_flags |= MNT_NOSUID; break;
		case Opt_suid:		mnt_flags &= ~MNT_NOSUID; break;
		case Opt_nodev:		mnt_flags |= MNT_NODEV; break;
		case Opt_dev:		mnt_flags &= ~MNT_NODEV; break;
		case Opt_noexec:	mnt_flags |= MNT_NOEXEC; break;
		case Opt_exec:		mnt_flags &= ~MNT_NOEXEC; break;
		case Opt_nodiratime:	mnt_flags |= MNT_NODIRATIME; break;
		case Opt_diratime:	mnt_flags &= ~MNT_NODIRATIME; break;
		case Opt_noatime:
			mnt_flags &= ~(MNT_RELATIME | MNT_NOATIME);
			mnt_flags |= MNT_NOATIME;
			break;
		case Opt_relatime:
			mnt_flags &= ~(MNT_RELATIME | MNT_NOATIME);
			mnt_flags |= MNT_RELATIME;
			break;
		case Opt_strictatime:
			mnt_flags &= ~(MNT_RELATIME | MNT_NOATIME);
			break;
		default:
			/* Unsupported vfs option */
			ret = -EINVAL;
			goto out;
		}
	}

	root = instance->root;
	ret = security_sb_may_mount(root->d_sb);
	if (ret)
		goto out;

	ret = -EPERM;
	if (mount_too_revealing(root, &mnt_flags))
		goto out;

	/* Add references that sb_mount will consume */
	atomic_inc(&root->d_sb->s_active);
	dget(root);
	path.mnt = sb_mount(root, instance->name, mnt_flags);
	ret = PTR_ERR(path.mnt);
	if (IS_ERR(path.mnt))
		goto out;

	/* Attach an O_PATH fd with a note that we need to unmount it,
	 * not just simply put it.
	 */
	path.dentry = dget(root);
	file = dentry_open(&path, O_PATH, current_cred());
	ret = PTR_ERR(file);
	if (IS_ERR(file))
		goto out;
	file->f_mode |= FMODE_NEED_UNMOUNT;

	ret = get_unused_fd_flags(O_CLOEXEC);
	if (ret >= 0)
		fd_install(ret, file);
	else
		fput(file);
out:
	path_put(&path);
	kfree(vfsv);
	kfree(fsv);
	kfree(options);
	fdput(f);
	return ret;
}

static char *seq_string(int (*show)(struct seq_file *seq, void *vp), void *vp)
{
	struct seq_file seq = { };

	mutex_init(&seq.lock);
	seq.buf = kvmalloc(seq.size = 8, GFP_KERNEL);
	if (!seq.buf)
		return ERR_PTR(-ENOMEM);

	for (;;) {
		int error = show(&seq, vp);
		if (error < 0) {
			kvfree(seq.buf);
			return ERR_PTR(error);
		}

		if (!seq_has_overflowed(&seq))
			break;

		kvfree(seq.buf);
		seq.buf = kvmalloc(seq.size <<= 1, GFP_KERNEL);
		seq.version = 0;
		seq.index = 0;
		seq.count = seq.from = 0;
	}
	seq.buf[seq.count] = '\0';
	return seq.buf;
}

static int seq_sb_options(struct seq_file *seq, void *vp)
{
	struct fs_instance *instance = vp;
	struct super_block *sb = instance->sb;
	unsigned long sb_flags = sb->s_flags;

	seq_printf(seq, "%s", sb_flags & SB_RDONLY ? "ro" : "rw");
	if (sb_flags & SB_SYNCHRONOUS)	seq_puts(seq, ",sync");
	if (sb_flags & SB_MANDLOCK)	seq_puts(seq, ",mand");
	if (sb_flags & SB_DIRSYNC)	seq_puts(seq, ",dirsync");
	if (sb_flags & SB_LAZYTIME)	seq_puts(seq, ",lazytime");
	if (sb_flags & SB_I_VERSION)	seq_puts(seq, ",iversion");

	if (sb->s_op->show_options) {
		struct dentry *root = instance->root;
		if (!root)
			root = sb->s_root;
		return sb->s_op->show_options(seq, root);
			root = instance->root;
	}

	return 0;
}

SYSCALL_DEFINE3(fsoptions,
		int, instancefd,
		char __user *, options,
		size_t, options_len)
{
	struct fs_instance *instance;
	char *sb_options = NULL;
	struct super_block *sb;
	struct fd f;
	ssize_t ret;
	size_t len;

	ret = -EBADF;
	f = fdget(instancefd);
	if (!f.file)
		goto out;
	if (f.file->f_op != &fs_instance_fops)
		goto out;
	instance = f.file->private_data;

	sb = instance->sb;
	down_read(&sb->s_umount);

	/* Is it too soon to read the options? */
	ret = -EBUSY;
	if ((sb->s_configure_seq <= 1) || !sb->s_root)
		goto out_unlock;

	instance->configure_seq = sb->s_configure_seq;
	sb_options = seq_string(seq_sb_options, instance);
	up_read(&sb->s_umount);

	ret = -ERANGE;
	len = strlen(sb_options) + 1;
	if (len > options_len)
		goto out;

	ret = -EFAULT;
	if (copy_to_user(options, sb_options, len))
		goto out;
	ret = len;
out:
	kvfree(sb_options);
	fdput(f);
	return ret;
out_unlock:
	up_read(&sb->s_umount);
	goto out;
}

static int seq_sb_name(struct seq_file *seq, void *vp)
{
	struct fs_instance *instance = vp;
	struct super_block *sb = instance->sb;
	struct dentry *root = instance->root;

	if (root && sb->s_op->show_devname)
		sb->s_op->show_devname(seq, root);
	else
		seq_printf(seq, "%s", instance->name);
	return 0;
}

SYSCALL_DEFINE3(fsname,
		int, instancefd,
		char __user *, name,
		size_t, name_len)
{
	struct fs_instance *instance;
	struct super_block *sb;
	char *sb_name = NULL;
	struct fd f;
	ssize_t ret;
	size_t len;

	ret = -EBADF;
	f = fdget(instancefd);
	if (!f.file)
		goto out;
	if (f.file->f_op != &fs_instance_fops)
		goto out;
	instance = f.file->private_data;
	sb = instance->sb;

	sb_name = seq_string(seq_sb_name, instance);
	ret = PTR_ERR(sb_name);
	if (IS_ERR(sb_name))
		goto out;

	ret = -ERANGE;
	len = strlen(sb_name) + 1;
	if (len > name_len)
		goto out_free;

	ret = -EFAULT;
	if (copy_to_user(name, sb_name, len))
		goto out_free;
	ret = len;
out_free:
	kvfree(sb_name);
out:
	fdput(f);
	return ret;
}

SYSCALL_DEFINE3(fstype,
		int, instancefd,
		char __user *, fstype,
		size_t, fstype_len)
{
	size_t type_len, subtype_len;
	struct fs_instance *instance;
	struct super_block *sb;
	struct fd f;
	ssize_t ret;

	ret = -EBADF;
	f = fdget(instancefd);
	if (!f.file)
		goto out;
	if (f.file->f_op != &fs_instance_fops)
		goto out;
	instance = f.file->private_data;
	sb = instance->sb;

	type_len = strlen(sb->s_type->name) +  1;
	subtype_len = 0;
	if (sb->s_subtype)
		subtype_len += strlen(sb->s_subtype) + 1;

	ret = -ERANGE;
	if ((type_len + subtype_len) > fstype_len)
		goto out;

	ret = -EFAULT;
	if (copy_to_user(fstype, sb->s_type->name, type_len))
		goto out;
	if (subtype_len) {
		char __user *dot = fstype + type_len - 1;
		if (put_user('.', dot))
			goto out;
		if (copy_to_user(fstype + type_len, sb->s_subtype, subtype_len))
			goto out;
	}
	ret = type_len + subtype_len;
out:
	fdput(f);
	return ret;
}
