/*
 * Copyright (c) 2013-2014 Motorola Mobility LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/proc_fs.h>
#include <linux/hashtable.h>
#include <linux/syscalls.h>
#include <linux/fcntl.h>
#include <linux/ctype.h>
#include <asm/uaccess.h>
#include "../internal.h"
#include "esdfs.h"

#define PKG_NAME_MAX		128
#define PKG_APPID_PER_USER	100000
#define PKG_APPID_MIN		1000
#define PKG_APPID_MAX		(PKG_APPID_PER_USER - 1)

static char *names_secure[] = {
	"autorun.inf",
	".android_secure",
	"android_secure",
	"" };

/* special path name searches */
static inline bool match_name(struct qstr *name, char *names[])
{
	int i = 0;

	BUG_ON(!name);
	for (i = 0; *names[i]; i++)
		if (name->len == strlen(names[i]) &&
		    !strncasecmp(names[i],
				 name->name,
				 name->len))
			return true;

	return false;
}

struct esdfs_package_list {
	struct hlist_node package_node;
	struct hlist_node access_node;
	char *name;
	unsigned long appid;
	unsigned access;
#define HAS_SDCARD_RW	(1 << 0)
#define HAS_MEDIA_RW	(1 << 1)
};

/*
 * Used for taking the raw package list in from user space.
 */
static struct proc_dir_entry *esdfs_proc_root;
static struct proc_dir_entry *esdfs_proc_packages;
static char *raw_package_list;
static unsigned long raw_package_list_size;

/*
 * The package list is global for all instances.  Since the entire list fits
 * into a few memory pages, keep it around as a means to store the package
 * names.  This is much more efficient than dynamically allocating heap for
 * each package.
 */
static unsigned num_packages;
static char *package_list_buffer;
static struct esdfs_package_list *package_list;
static DEFINE_HASHTABLE(package_list_hash, 8);
static DEFINE_HASHTABLE(access_list_hash, 7);
static DEFINE_MUTEX(package_list_lock);

unsigned esdfs_package_list_version;

/*
 * Parse the raw package list, which is one package per line with each element
 * separated by a single white space.  Skip lines that do not parse correctly.
 */
static int parse_package_list(char *buffer, unsigned long size)
{
	char *next_line = buffer;
	char *sep, *sepres, *name;
	unsigned long appid, gid;
	unsigned hash, access;
	unsigned count = 0, line = 0, pi = 0;
	struct esdfs_package_list *pl = NULL;
	int ret, err = -EINVAL;

	if (!buffer || size == 0)
		return -EINVAL;

	while ((next_line = strnchr(next_line, size, '\n'))) {
		count++;
		next_line++;
	}

	pr_debug("esdfs: %s: package list: %lu bytes, %d lines\n",
		__func__, size, count);
	if (count == 0)
		return -EINVAL;
	pl = kzalloc(count * sizeof(struct esdfs_package_list), GFP_KERNEL);
	if (!pl)
		return -ENOMEM;

	next_line = buffer;
	sep = strsep(&next_line, "\n");
	while (next_line && sep && line < count) {
		line++;
		err = -EINVAL;
		name = strsep(&sep, " ");
		if (!sep)
			goto next;

		sepres = strsep(&sep, " ");
		if (!sep)
			goto next;
		ret = kstrtoul(sepres, 0, &appid);
		if (ret) {
			err = ret;
			goto next;
		}

		strsep(&sep, " ");
		if (!sep)
			goto next;
		strsep(&sep, " ");
		if (!sep)
			goto next;
		strsep(&sep, " ");
		if (!sep)
			goto next;

		sepres = strsep(&sep, ",");
		while (sepres) {
			gid = 0;
			if (kstrtoul(sepres, 0, &gid) == 0) {
				if (gid == AID_SDCARD_RW)
					access |= HAS_SDCARD_RW;
				else if (gid == AID_MEDIA_RW)
					access |= HAS_MEDIA_RW;
			}
			sepres = strsep(&sep, ",");
		}
		pr_debug("esdfs: %s: %s, %lu, 0x%02X\n",
			 __func__, name, appid, access);

		/* Parsed the line OK, so do some sanity checks. */
		if (strlen(name) > PKG_NAME_MAX - 1 ||
		    appid < PKG_APPID_MIN || appid > PKG_APPID_MAX)
			goto next;

		err = 0;
		pl[pi].name = name;
		pl[pi].appid = appid;
		pl[pi].access = access;
		pi++;
next:
		if (err)
			pr_err("esdfs: %s: package list parse error on line %d: %d\n",
				__func__, line, err);
		appid = 0;
		access = 0;
		sep = strsep(&next_line, "\n");
	}
	count = pi;

	pr_debug("esdfs: %s: parsed %d packages\n", __func__, count);

	/* Commit the new list */
	mutex_lock(&package_list_lock);

	hash_init(package_list_hash);
	hash_init(access_list_hash);

	for (pi = 0; pi < count; pi++) {
		hash = full_name_hash(pl[pi].name, strlen(pl[pi].name));
		hash_add(package_list_hash, &pl[pi].package_node, hash);
		if (pl[pi].access)
			hash_add(access_list_hash, &pl[pi].access_node,
				 pl[pi].appid);
		pr_debug("esdfs: %s: %s (0x%08x), %lu, 0x%02X\n", __func__,
			pl[pi].name, hash, pl[pi].appid, pl[pi].access);
	}

	num_packages = count;
	kfree(package_list);
	package_list = pl;
	kfree(package_list_buffer);
	package_list_buffer = buffer;
	esdfs_package_list_version++;

	mutex_unlock(&package_list_lock);

	return 0;
}

static int proc_packages_write(struct file *file, const char *chunk,
				     unsigned long count, void *data)
{
	char *buffer;
	int err;

	buffer = kmalloc(count + raw_package_list_size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	if (raw_package_list) {
		memcpy(buffer, raw_package_list, raw_package_list_size);
		kfree(raw_package_list);
		raw_package_list_size = 0;
		raw_package_list = NULL;
	}

	if (copy_from_user(buffer + raw_package_list_size, chunk, count)) {
		kfree(buffer);
		pr_err("esdfs: %s: :(\n", __func__);
		return -EFAULT;
	}
	raw_package_list_size += count;
	raw_package_list = buffer;

	/*  The list is terminated by an empty line. */
	if (raw_package_list[raw_package_list_size - 2] == '\n' &&
	    raw_package_list[raw_package_list_size - 1] == '\n') {
		raw_package_list[raw_package_list_size - 1] = '\0';
		err = parse_package_list(raw_package_list,
					 raw_package_list_size);
		raw_package_list_size = 0;
		raw_package_list = NULL;
		if (err)
			pr_err("esdfs: %s: failed to parse package list: %d\n",
			       __func__, err);
		else
			pr_debug("esdfs: %s: package list loaded successfully\n",
				__func__);
	}

	return count;
}

int esdfs_init_package_list(void)
{
	if (!esdfs_proc_root)
		esdfs_proc_root = proc_mkdir("fs/esdfs", NULL);
	if (esdfs_proc_root && !esdfs_proc_packages)
		esdfs_proc_packages =
			create_proc_entry("packages", S_IWUSR, esdfs_proc_root);
	if (esdfs_proc_packages)
		esdfs_proc_packages->write_proc = proc_packages_write;

	return 0;
}

void esdfs_destroy_package_list(void)
{
	if (esdfs_proc_packages)
		remove_proc_entry("fs/esdfs/packages", NULL);
	if (esdfs_proc_root)
		remove_proc_entry("fs/esdfs", NULL);
}

/*
 * Derive an entry's premissions tree position based on its parent.
 */
void esdfs_derive_perms(struct dentry *dentry)
{
	struct esdfs_inode_info *inode_i = ESDFS_I(dentry->d_inode);
	bool is_root;
	struct esdfs_package_list *package;
	struct hlist_node *tmp;
	unsigned hash;
	int ret;

	spin_lock(&dentry->d_lock);
	is_root = IS_ROOT(dentry);
	spin_unlock(&dentry->d_lock);
	if (is_root)
		return;

	/* Inherit from the parent to start */
	inode_i->tree = ESDFS_I(dentry->d_parent->d_inode)->tree;
	inode_i->userid = ESDFS_I(dentry->d_parent->d_inode)->userid;
	inode_i->appid = ESDFS_I(dentry->d_parent->d_inode)->appid;

	/*
	 * ESDFS_TREE_MEDIA* are intentionally dead ends.
	 */
	switch (inode_i->tree) {
	case ESDFS_TREE_ROOT_LEGACY:
		inode_i->tree = ESDFS_TREE_ROOT;
		if (!strncasecmp(dentry->d_name.name,
					"obb",
					dentry->d_name.len))
			inode_i->tree = ESDFS_TREE_ANDROID_OBB;
		break;

	case ESDFS_TREE_ROOT:
		inode_i->tree = ESDFS_TREE_MEDIA;
		ret = kstrtou32(dentry->d_name.name, 0, &inode_i->userid);
		if (!strncasecmp(dentry->d_name.name,
				 "Android",
				 dentry->d_name.len))
			inode_i->tree = ESDFS_TREE_ANDROID;
		break;

	case ESDFS_TREE_ANDROID:
		if (!strncasecmp(dentry->d_name.name,
				 "data",
				 dentry->d_name.len))
			inode_i->tree = ESDFS_TREE_ANDROID_DATA;
		else if (!strncasecmp(dentry->d_name.name,
					"obb",
					dentry->d_name.len))
			inode_i->tree = ESDFS_TREE_ANDROID_OBB;
		else if (test_opt(ESDFS_SB(dentry->d_sb), DERIVE_UNIFIED) &&
			   !strncasecmp(dentry->d_name.name,
					"user",
					dentry->d_name.len))
			inode_i->tree = ESDFS_TREE_ANDROID_USER;
		break;

	case ESDFS_TREE_ANDROID_DATA:
	case ESDFS_TREE_ANDROID_OBB:
		hash = full_name_hash(dentry->d_name.name, dentry->d_name.len);
		mutex_lock(&package_list_lock);
		hash_for_each_possible(package_list_hash, package, tmp,
				       package_node, hash) {
			if (!strncmp(package->name, dentry->d_name.name,
				     dentry->d_name.len)) {
				inode_i->appid = package->appid;
				break;
			}
		}
		mutex_unlock(&package_list_lock);
		inode_i->tree = ESDFS_TREE_ANDROID_APP;
		break;

	case ESDFS_TREE_ANDROID_USER:
		/* Another user, so start over */
		inode_i->tree = ESDFS_TREE_ROOT;
		ret = kstrtou32(dentry->d_name.name, 0, &inode_i->userid);
		break;
	}
}

/* Apply tree position-specific permissions */
void esdfs_set_derived_perms(struct inode *inode)
{
	struct esdfs_sb_info *sbi = ESDFS_SB(inode->i_sb);
	struct esdfs_inode_info *inode_i = ESDFS_I(inode);

	inode->i_uid = sbi->upper_perms.uid;
	inode->i_gid = sbi->upper_perms.gid;
	inode->i_mode &= S_IFMT;

	switch (inode_i->tree) {
	case ESDFS_TREE_ROOT_LEGACY:
		inode->i_mode |= sbi->upper_perms.dmask;
		break;

	case ESDFS_TREE_NONE:
	case ESDFS_TREE_ROOT:
		inode->i_gid = AID_SDCARD_R;
		inode->i_mode |= sbi->upper_perms.dmask;
		break;

	case ESDFS_TREE_MEDIA:
		inode->i_gid = AID_SDCARD_R;
		inode->i_mode |= 0770;
		break;

	case ESDFS_TREE_MEDIA_PICS:
		inode->i_gid = AID_SDCARD_PICS;
		inode->i_mode |= 0770;
		break;

	case ESDFS_TREE_MEDIA_AV:
		inode->i_gid = AID_SDCARD_AV;
		inode->i_mode |= 0770;
		break;

	case ESDFS_TREE_ANDROID:
	case ESDFS_TREE_ANDROID_DATA:
	case ESDFS_TREE_ANDROID_OBB:
		inode->i_mode |= 0771;
		break;

	case ESDFS_TREE_ANDROID_APP:
		if (inode_i->appid)
			inode->i_uid = inode_i->userid * PKG_APPID_PER_USER +
				       (inode_i->appid % PKG_APPID_PER_USER);
		inode->i_mode |= 0770;
		break;

	case ESDFS_TREE_ANDROID_USER:
		inode->i_gid = AID_SDCARD_ALL;
		inode->i_mode |= 0770;
		break;
	}

	/* strip execute bits from any non-directories */
	if (!S_ISDIR(inode->i_mode))
		inode->i_mode &= ~S_IXUGO;
}

int esdfs_derived_lookup(struct dentry *dentry, struct dentry **parent)
{
	/* Deny access to security-sensitive entries */
	if (ESDFS_I((*parent)->d_inode)->tree == ESDFS_TREE_ROOT &&
	    match_name(&dentry->d_name, names_secure)) {
		pr_debug("esdfs: denying access to: %s", dentry->d_name.name);
		return -EACCES;
	}

	/* Handle obb directory "grafting" as a sort of hard link. */
	if (ESDFS_I((*parent)->d_inode)->tree == ESDFS_TREE_ANDROID &&
	    !strncasecmp(dentry->d_name.name, "obb", dentry->d_name.len)) {
		if (test_opt(ESDFS_SB((*parent)->d_sb), DERIVE_LEGACY))
			*parent = dget(dentry->d_sb->s_root);
		/* FIXME: unified mode for SD cards */
	}
	return 0;
}

/*
 * Implement the extra checking that is done based on the caller's package
 * list-based access rights.
 */
int esdfs_check_derived_permission(struct inode *inode, int mask)
{
	const struct cred *cred;
	struct esdfs_package_list *package;
	struct hlist_node *tmp;
	uid_t appid;
	unsigned access = 0;

	cred = current_cred();
	appid = cred->uid % PKG_APPID_PER_USER;

	/* Reads, owners, and root are always granted access */
	if (!(mask & MAY_WRITE) || cred->uid == 0 || cred->uid == inode->i_uid)
		return 0;

	/*
	 * Since Android now allows sdcard_r access to the tree and it does not
	 * know how to use extended attributes, we have to double-check write
	 * requests against the list of apps that have been granted sdcard_rw.
	 */
	mutex_lock(&package_list_lock);
	hash_for_each_possible(access_list_hash, package, tmp,
			       access_node, appid) {
		if (package->appid == appid) {
			pr_debug("esdfs: %s: found appid %lu, access: %u\n",
				__func__, package->appid, package->access);
			access = package->access;
			break;
		}
	}
	mutex_unlock(&package_list_lock);

	/*
	 * Grant access to media_rw holders (they can access the source anyway).
	 */
	if (access & HAS_MEDIA_RW)
		return 0;

	/*
	 * Grant access to sdcard_rw holders, unless we are in unified mode
	 * and we are trying to write to the protected /Android tree.
	 */
	if ((access & HAS_SDCARD_RW) &&
	    (!test_opt(ESDFS_SB(inode->i_sb), DERIVE_UNIFIED) ||
	     (ESDFS_I(inode)->tree != ESDFS_TREE_ROOT &&
	      ESDFS_I(inode)->tree != ESDFS_TREE_ANDROID &&
	      ESDFS_I(inode)->tree != ESDFS_TREE_ANDROID_DATA &&
	      ESDFS_I(inode)->tree != ESDFS_TREE_ANDROID_OBB &&
	      ESDFS_I(inode)->tree != ESDFS_TREE_ANDROID_APP)))
		return 0;

	pr_debug("esdfs: %s: denying access to appid: %d", __func__, appid);
	return -EACCES;
}

/*
 * The sdcard service has a hack that creates .nomedia files along certain
 * paths to stop MediaScanner.  Create those here.
 */
int esdfs_derive_mkdir_contents(struct dentry *dir_dentry)
{
	struct esdfs_inode_info *inode_i = ESDFS_I(dir_dentry->d_inode);
	struct qstr nomedia;
	struct dentry *lower_dentry;
	struct path lower_path;
	struct nameidata nd;
	umode_t mode;
	int err = 0;

	if (inode_i->tree != ESDFS_TREE_ANDROID_DATA &&
	    inode_i->tree != ESDFS_TREE_ANDROID_OBB)
		return 0;

	nomedia.name = ".nomedia";
	nomedia.len = strlen(nomedia.name);
	nomedia.hash = full_name_hash(nomedia.name, nomedia.len);

	esdfs_get_lower_path(dir_dentry, &lower_path);

	/* If it's in the cache already, there is no reason to create it. */
	lower_dentry = d_lookup(lower_path.dentry, &nomedia);
	if (lower_dentry) {
		dput(lower_dentry);
		goto out;
	}

	/* Create a negative, lower dentry. */
	lower_dentry = d_alloc(lower_path.dentry, &nomedia);
	if (!lower_dentry) {
		err = -ENOMEM;
		goto out;
	}
	d_add(lower_dentry, NULL);

	/* Create the lower file. */
	nd.path.dentry = lower_dentry;
	mode = S_IFREG;
	esdfs_set_lower_mode(ESDFS_SB(dir_dentry->d_sb), &mode);
	err = vfs_create(lower_path.dentry->d_inode, lower_dentry, mode, &nd);
	dput(lower_dentry);

out:
	esdfs_put_lower_path(dir_dentry, &lower_path);

	return err;
}
