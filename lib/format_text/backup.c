/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "format-text.h"

#include "log.h"
#include "pool.h"
#include "config.h"
#include "hash.h"
#include "import-export.h"
#include "lvm-string.h"

#include <dirent.h>
#include <unistd.h>


/*
 * The format instance is given a directory path
 * upon creation.  Each file in this directory
 * whose name is of the form '(.*)_[0-9]*.vg' is a config
 * file (see lib/config.[hc]), which contains a
 * description of a single volume group.
 *
 * The prefix ($1 from the above regex) of the
 * config file gives the volume group name.
 *
 * Backup files that have expired will be removed.
 */

struct backup_c {
	uint32_t retain_days;
	uint32_t min_retains;

	char *dir;

	/*
	 * An ordered list of previous backups.
	 * Each list entered against the vg name.
	 * Most recent first.
	 */
	struct hash_table *vg_backups;

	/*
	 * Scratch pool.  Contents of vg_backups
	 * come from here.
	 */
	struct pool *mem;
};

/*
 * A list of these is built up for each volume
 * group.  Ordered with the least recent at the
 * head.
 */
struct backup_file {
	struct list list;

	char *path;
	char *vg;
	int index;
};

/*
 * This format is write only.
 */
static void _unsupported(const char *cmd)
{
	log_err("The backup format doesn't support '%s'", cmd);
}

static struct list *_get_vgs(struct format_instance *fi)
{
	_unsupported("get_vgs");
	return NULL;
}

static struct list *_get_pvs(struct format_instance *fi)
{
	_unsupported("get_pvs");
	return NULL;
}

static struct physical_volume *_pv_read(struct format_instance *fi,
					const char *pv_name)
{
	_unsupported("pv_read");
	return NULL;
}

static int _pv_setup(struct format_instance *fi, struct physical_volume *pv,
	     struct volume_group *vg)
{
	_unsupported("pv_setup");
	return 0;
}

static int _pv_write(struct format_instance *fi, struct physical_volume *pv)
{
	_unsupported("pv_write");
	return 0;
}

static int _vg_setup(struct format_instance *fi, struct volume_group *vg)
{
	_unsupported("vg_setup");
	return 0;
}

static struct volume_group *_vg_read(struct format_instance *fi,
				     const char *vg_name)
{
	_unsupported("vg_read");
	return NULL;
}

static void _destroy(struct format_instance *fi)
{
	struct backup_c *bc = (struct backup_c *) fi->private;
	if (bc->vg_backups)
		hash_destroy(bc->vg_backups);
	pool_destroy(bc->mem);
}


/*
 * vg_write implementation starts here.
 */
static int _split_vg(const char *filename, char *vg, size_t vg_size,
		     uint32_t *index)
{
	char buffer[64];
	int n;

	snprintf(buffer, sizeof(buffer), "\%%ds_\%u.vg%n", vg_size, &n);
	return (sscanf(filename, buffer, vg, index) == 2) &&
		(filename + n == '\0');
}

static void _insert_file(struct list *head, struct backup_file *b)
{
	struct list *bh;
	struct backup_file *bf;

	if (list_empty(head)) {
		list_add(head, &b->list);
		return;
	}

	list_iterate (bh, head) {
		bf = list_item(bh, struct backup_file);

		if (bf->index < b->index)
			break;
	}

	list_add_h(&bf->list, &b->list);
}

static int _scan_vg(struct backup_c *bc, const char *file,
		    const char *vg_name, int index)
{
	struct backup_file *b;
	struct list *files;

	/*
	 * Do we need to create a new list of
	 * backup files for this vg ?
	 */
	if (!(files = hash_lookup(bc->vg_backups, vg_name))) {
		if (!(files = pool_alloc(bc->mem, sizeof(*files)))) {
			stack;
			return 0;
		}

		list_init(files);
		if (!hash_insert(bc->vg_backups, vg_name, files)) {
			log_err("Couldn't insert backup file "
				"into hash table.");
			return 0;
		}
	}

	/*
	 * Create a new backup file.
	 */
	if (!(b = pool_alloc(bc->mem, sizeof(*b)))) {
		log_err("Couldn't create new backup file.");
		return 0;
	}

	/*
	 * Insert it to the correct part of the
	 * list.
	 */
	_insert_file(files, b);

	return 1;
}

static char *_join(struct pool *mem, const char *dir, const char *name)
{
	if (!pool_begin_object(mem, 32) ||
	    !pool_grow_object(mem, dir, strlen(dir)) ||
	    !pool_grow_object(mem, "/", 1) ||
	    !pool_grow_object(mem, name, strlen(name)) ||
	    !pool_grow_object(mem, "\0", 1)) {
		stack;
		return NULL;
	}

	return pool_end_object(mem);
}

static int _scan_dir(struct backup_c *bc)
{
	int r = 0, i, count, index;
	char vg_name[64], *path;
	struct dirent **dirent;

	if ((count = scandir(bc->dir, &dirent, NULL, alphasort)) < 0) {
		log_err("Couldn't scan backup directory.");
		return 0;
	}

	for (i = 0; i < count; i++) {
		if ((dirent[i]->d_name[0] == '.') ||
		    !_split_vg(dirent[i]->d_name, vg_name,
			       sizeof(vg_name), &index))
			continue;

		if (!(path = _join(bc->mem, bc->dir, dirent[i]->d_name))) {
			stack;
			goto out;
		}

		_scan_vg(bc, path, vg_name, index);
	}
	r = 1;

 out:
	for (i = 0; i < count; i++)
		free(dirent[i]);
	free(dirent);

	return r;
}

static int _scan_backups(struct backup_c *bc)
{
	pool_empty(bc->mem);

	if (bc->vg_backups)
		hash_destroy(bc->vg_backups);

	if (!(bc->vg_backups = hash_create(128))) {
		log_err("Couldn't create hash table for scanning backups.");
		return 0;
	}

	if (!_scan_dir(bc)) {
		stack;
		return 0;
	}

	return 1;
}

static int _vg_write(struct format_instance *fi, struct volume_group *vg)
{
	int r = 0, index = 0, i, fd;
	struct backup_c *bc = (struct backup_c *) fi->private;
	struct backup_file *last;
	char *tmp_name;
	FILE *fp = NULL;
	char backup_name[PATH_MAX];

	/*
	 * Build a format string for mkstemp.
	 */
	if (lvm_snprintf(backup_name, sizeof(backup_name), "%s/lvm_XXXXXX",
			 bc->dir) < 0) {
		log_err("Couldn't generate template for backup name.");
		return 0;
	}

	/*
	 * Write the backup, to a temporary file.
	 */
	if ((fd = mkstemp(backup_name)) == -1) {
		log_err("Couldn't create temporary file for backup.");
		return 0;
	}

	if (!(fp = fdopen(fd, "w"))) {
		log_err("Couldn't create FILE object for backup.");
		close(fd);
		return 0;
	}

	if (!text_vg_export(fp, vg)) {
		stack;
		goto out;
	}

	/*
	 * Now we want to rename this file to <vg>_index.vg.
	 */
	if (!_scan_backups(bc)) {
		log_err("Couldn't scan the backup directory (%s).", bc->dir);
		goto out;
	}

	if ((last = (struct backup_file *) hash_lookup(bc->vg_backups,
						       vg->name))) {
		/* move to the last in the list */
		last = list_item(last->list.p, struct backup_file);
		index = last->index + 1;
	}

	for (i = 0; i < 10; i++) {
		if (lvm_snprintf(backup_name, sizeof(backup_name),
				 "%s/%s_%d.vg",
				 bc->dir, vg->name, index) < 0) {
			log_err("backup file name too long.");
			goto out;
		}
#if 0
		if (rename(tmp_name, backup_name) < 0) {
			log_err("couldn't rename backup file to %s.",
				backup_name);
		} else {
			r = 1;
			break;
		}
#else
		r = 1;
		break;
#endif

		index++;
	}

 out:
	if (fp)
		fclose(fp);
	free(tmp_name);
	return r;
}

void backup_expire(struct format_instance *fi)
{
	/* FIXME: finish */
}

static struct format_handler _backup_handler = {
	get_vgs: _get_vgs,
	get_pvs: _get_pvs,
	pv_read: _pv_read,
	pv_setup: _pv_setup,
	pv_write: _pv_write,
	vg_setup: _vg_setup,
	vg_read: _vg_read,
	vg_write: _vg_write,
	destroy: _destroy
};

struct format_instance *backup_format_create(struct cmd_context *cmd,
					     const char *dir,
					     uint32_t retain_days,
					     uint32_t min_retains)
{
	struct format_instance *fi;
	struct backup_c *bc = NULL;
	struct pool *mem = cmd->mem;

	if (!(bc = pool_zalloc(mem, sizeof(*bc)))) {
		stack;
		return NULL;
	}

	if (!(bc->mem = pool_create(1024))) {
		stack;
		goto bad;
	}

	if (!(bc->dir = pool_strdup(mem, dir))) {
		stack;
		goto bad;
	}

	bc->retain_days = retain_days;
	bc->min_retains = min_retains;

	if (!(fi = pool_alloc(mem, sizeof(*fi)))) {
		stack;
		goto bad;
	}

	fi->cmd = cmd;
	fi->ops = &_backup_handler;
	fi->private = bc;

	return fi;

 bad:
	if (bc->mem)
		pool_destroy(bc->mem);

	pool_free(mem, bc);
	return NULL;
}
