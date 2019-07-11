/* cacheutils.c - crash extension module for dumping page caches
 *
 * Copyright (C) 2019 NEC Corporation
 *
 * Author: Kazuhito Hagio <k-hagio@ab.jp.nec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "defs.h"

#define CU_INVALID_MEMBER(X)	(cu_offset_table.X == INVALID_OFFSET)
#define CU_OFFSET_INIT(X, Y, Z)	(cu_offset_table.X = MEMBER_OFFSET(Y, Z))
#define CU_OFFSET(X)	(OFFSET_verify(cu_offset_table.X, \
			(char *)__FUNCTION__, __FILE__, __LINE__, #X))

struct cu_offset_table {
	long inode_i_size;
	long vfsmount_mnt_root;
	long dentry_d_subdirs;
	long dentry_d_child;
};
static struct cu_offset_table cu_offset_table = {};

void cacheutils_init(void);
void cacheutils_fini(void);
void cmd_ccat(void);
void cmd_cls(void);
void cmd_cfind(void);

#define DUMP_CACHES		(0x0001)
#define DUMP_DONT_SEEK		(0x0002)
#define SHOW_INFO		(0x0010)
#define SHOW_INFO_DIRS		(0x0020)
#define SHOW_INFO_NEG_DENTS	(0x0040)
#define SHOW_INFO_DONT_SORT	(0x0080)
#define FIND_FILES		(0x0100)
#define FIND_COUNT_DENTRY	(0x0200)

static char *header_fmt = "%-16s %-16s %-16s %7s %3s %s\n";
static char *dentry_fmt = "%-16lx %-16lx %-16lx %7lu %3d %s%s\n";
static char *negdent_fmt = "%-16lx %-16s %-16s %7s %3s %s\n";
static char *count_header_fmt = "%7s %6s %6s %s\n";
static char *count_dentry_fmt = "%7d %6d %6d %s\n";

/* Global variables */
static int flags;
static FILE *outfp;
static char *pgbuf;
static ulong nr_written, nr_excluded;
static ulonglong i_size;
static struct task_context *tc;
static int total_dentry, total_negdent;

/* Per-command caches */
static int mount_count;
static char *mount_data;
static char **mount_path;
static char *dentry_data;

static int
dump_slot(ulong slot)
{
	physaddr_t phys;
	ulong index, pos, size;

	if (!is_page_ptr(slot, &phys))
		return FALSE;

	if (!readmem(slot + OFFSET(page_index), KVADDR, &index,
	    sizeof(ulong), "page.index", RETURN_ON_ERROR))
		return FALSE;

	/*
	 * If the page content was excluded by makedumpfile,
	 * skip it quietly.
	 */
	if (!readmem(phys, PHYSADDR, pgbuf,
	    PAGESIZE(), "page content", RETURN_ON_ERROR|QUIET)) {
		nr_excluded++;
		return TRUE;
	}

	pos = index * PAGESIZE();
	size = (pos + PAGESIZE()) > i_size ? i_size - pos : PAGESIZE();

	if (!(flags & DUMP_DONT_SEEK))
		fseek(outfp, pos, SEEK_SET);

	if (fwrite(pgbuf, sizeof(char), size, outfp) == size)
		nr_written++;
	else if (errno != EPIPE || CRASHDEBUG(1))
		error(INFO, "%lx: write error: %s\n", slot, strerror(errno));

	return TRUE;
}

/*
 * NOTE: Return a static address. No need to FREEBUF(), but need to
 * strdup() or strncpy() if we want to get another dentry's name with
 * this function before consuming the former one.
 */
static char *
get_dentry_name(ulong dentry, char *dentry_buf)
{
	ulong d_name_name, d_iname;
	static char name[NAME_MAX+1];
	static char unknown[] = "(unknown)";

	BZERO(name, sizeof(name));

	d_name_name = ULONG(dentry_buf + OFFSET(dentry_d_name)
					+ OFFSET(qstr_name));
	d_iname = dentry + OFFSET(dentry_d_iname);

	/*
	 * d_name.name and d_iname are guaranteed NUL-terminated.
	 * See d_alloc() in the kernel.
	 */
	if (d_name_name == d_iname)
		strncpy(name, dentry_buf + OFFSET(dentry_d_iname), NAME_MAX+1);
	else
		if (!readmem(d_name_name, KVADDR, name, NAME_MAX+1,
		    "dentry.d_name.name", RETURN_ON_ERROR))
			return unknown;

	return name;
}

static int
get_inode_info(ulong inode, uint *i_mode, ulong *i_mapping,
		ulonglong *i_size, ulong *nrpages)
{
	char *inode_buf;
	int ret = FALSE;

	inode_buf = GETBUF(SIZE(inode));

	if (!readmem(inode, KVADDR, inode_buf, SIZE(inode),
	    "inode buffer", RETURN_ON_ERROR))
		goto bail_out;

	if (i_mode) {
		if (SIZE(umode_t) == SIZEOF_32BIT)
			*i_mode = UINT(inode_buf + OFFSET(inode_i_mode));
		else
			*i_mode = USHORT(inode_buf + OFFSET(inode_i_mode));
	}
	if (i_mapping)
		*i_mapping = ULONG(inode_buf + OFFSET(inode_i_mapping));
	if (i_size)
		*i_size = ULONGLONG(inode_buf + CU_OFFSET(inode_i_size));
	if (nrpages)
		if (!readmem(*i_mapping + OFFSET(address_space_nrpages),
		    KVADDR, nrpages, sizeof(ulong), "i_mapping.nrpages",
		    RETURN_ON_ERROR))
			goto bail_out;

	ret = TRUE;
bail_out:
	FREEBUF(inode_buf);
	return ret;
}

static ulong *
get_subdirs_list(int *cntptr, ulong dentry)
{
	struct list_data list_data, *ld;
	ulong d_subdirs, child;

	d_subdirs = dentry + CU_OFFSET(dentry_d_subdirs);

	if (!readmem(d_subdirs, KVADDR, &child, sizeof(ulong),
	    "dentry.d_subdirs", RETURN_ON_ERROR))
		return NULL;

	if (d_subdirs == child)
		return NULL;

	ld = &list_data;
	BZERO(ld, sizeof(struct list_data));
	ld->flags |= (LIST_ALLOCATE|RETURN_ON_LIST_ERROR);
	ld->start = child;
	ld->end = d_subdirs;
	ld->list_head_offset = CU_OFFSET(dentry_d_child);
	if (CRASHDEBUG(3))
		ld->flags |= VERBOSE;

	if ((*cntptr = do_list(ld)) == -1)
		return NULL;

	return ld->list_ptr;
}

static char *
get_type_indicator(uint i_mode)
{
	static char c[2] = {'\0', '\0'};

	if (S_ISREG(i_mode)) {
		if (i_mode & (S_IXUSR|S_IXGRP|S_IXOTH))
			*c = '*';
		else
			*c = '\0';
	} else if (S_ISDIR(i_mode))
		*c = '/';
	else if (S_ISLNK(i_mode))
		*c = '@';
	else if (S_ISFIFO(i_mode))
		*c = '|';
	else if (S_ISSOCK(i_mode))
		*c = '=';
	else
		*c = '\0';

	return c;
}

static ulonglong
byte_to_page(ulonglong i_size)
{
	return (i_size / PAGESIZE()) + ((i_size % PAGESIZE()) ? 1 : 0);
}

static int
calc_cached_percent(ulong nrpages, ulonglong i_size)
{
	if (!i_size)
		return 0;

	return (nrpages * 100) / byte_to_page(i_size);
}

typedef struct {
	ulong dentry;
	char *name;
	ulong inode;
	ulong i_mapping;
	ulonglong i_size;
	ulong nrpages;
	uint i_mode;
} inode_info_t;

static int
sort_by_name(const void *arg1, const void *arg2)
{
	inode_info_t *p = (inode_info_t *)arg1;
	inode_info_t *q = (inode_info_t *)arg2;

	return strcmp(p->name, q->name);
}

static void
show_subdirs_info(ulong dentry)
{
	ulong *list;
	int i, count;
	char *dentry_buf;
	ulong d, inode, i_mapping, nrpages;
	uint i_mode;
	ulonglong i_size;
	inode_info_t *inode_list, *p;

	if (!(list = get_subdirs_list(&count, dentry)))
		return;

	inode_list = (inode_info_t *)GETBUF(sizeof(inode_info_t) * count);

	for (i = 0, p = inode_list; i < count; i++) {
		d = list[i];
		dentry_buf = fill_dentry_cache(d);

		inode = ULONG(dentry_buf + OFFSET(dentry_d_inode));
		if (inode && get_inode_info(inode, &i_mode, &i_mapping,
					&i_size, &nrpages)) {
			p->inode = inode;
			p->i_mapping = i_mapping;
			p->i_size = i_size;
			p->nrpages = nrpages;
			p->i_mode = i_mode;
		} else {
			p->i_mapping = 0;
			if (!(flags & SHOW_INFO_NEG_DENTS))
				continue;
		}
		p->dentry = d;
		p->name = strdup(get_dentry_name(d, dentry_buf));
		p++;
	}
	count = p - inode_list;

	if (!(flags & SHOW_INFO_DONT_SORT))
		qsort(inode_list, count, sizeof(inode_info_t), sort_by_name);

	for (i = 0, p = inode_list; i < count; i++, p++) {
		if (p->i_mapping) {
			int pct = calc_cached_percent(p->nrpages, p->i_size);

			fprintf(fp, dentry_fmt, p->dentry, p->inode,
				p->i_mapping, p->nrpages, pct, p->name,
				get_type_indicator(p->i_mode));

			if (CRASHDEBUG(1))
				fprintf(fp, "  i_mode:%6o i_size:%llu (%llu)\n",
					p->i_mode, p->i_size,
					byte_to_page(p->i_size));

		} else if (flags & SHOW_INFO_NEG_DENTS)
			fprintf(fp, negdent_fmt,
				p->dentry, "-", "-", "-", "-", p->name);
		free(p->name);
	}

	FREEBUF(inode_list);
	FREEBUF(list);
}

/*
 * If remaining_path is NULL, search for a mount point that matches exactly
 * with the path.
 */
static ulong
get_mntpoint_dentry(char *path, char **remaining_path)
{
	ulong *mount_list;
	int i;
	size_t len;
	char *mount_buf, *path_buf, *path_start, *slash_pos;
	char buf[PATH_MAX], *bufp = buf;
	ulong root, parent, mountp;
	long size;

	size = VALID_STRUCT(mount) ? SIZE(mount) : SIZE(vfsmount);
	if (!mount_data) {
		mount_list = get_mount_list(&mount_count, tc);
		mount_data = GETBUF(size * mount_count);
		mount_path = (char **)GETBUF(sizeof(char *) * mount_count);

		for (i = 0; i < mount_count; i++) {
			if (!readmem(mount_list[i], KVADDR, mount_data +
			    (size * i), size, "(vfs)mount buffer",
			    RETURN_ON_ERROR)) {
				FREEBUF(mount_list);
				goto bail_out;
			}

			mount_buf = mount_data + (size * i);

			if (VALID_STRUCT(mount)) {
				parent = ULONG(mount_buf +
					OFFSET(mount_mnt_parent));
				mountp = ULONG(mount_buf +
					OFFSET(mount_mnt_mountpoint));
				get_pathname(mountp, bufp, BUFSIZE, 1,
					parent + OFFSET(mount_mnt));
			} else {
				parent = ULONG(mount_buf +
					OFFSET(vfsmount_mnt_parent));
				mountp = ULONG(mount_buf +
					OFFSET(vfsmount_mnt_mountpoint));
				get_pathname(mountp, bufp, BUFSIZE, 1,
					parent);
			}

			len = strnlen(bufp, PATH_MAX);
			mount_path[i] = GETBUF(len + 1);
			memcpy(mount_path[i], bufp, len + 1);
		}
		FREEBUF(mount_list);
	}

	len = strlen(path);
	path_buf = GETBUF(len + 1);
	memcpy(path_buf, path, len + 1);

	path_start = path + len;

	root = 0;
	while (TRUE) {
		for (i = 0; i < mount_count; i++) {
			mount_buf = mount_data + (size * i);
			bufp = mount_path[i];

			if (CRASHDEBUG(2))
				error(INFO, "path:%s PATHEQ:%d mntp:%s\n",
					path_buf, PATHEQ(path_buf, bufp), bufp);

			if (PATHEQ(path_buf, bufp)) {
				if (VALID_STRUCT(mount))
					root = ULONG(mount_buf +
						OFFSET(mount_mnt) +
						CU_OFFSET(vfsmount_mnt_root));
				else
					root = ULONG(mount_buf +
						CU_OFFSET(vfsmount_mnt_root));
				/*
				 * Probably the last one will be what we want,
				 * so don't break here.
				 */
			}
		}
		if (root)
			break;

		if (!remaining_path) /* exact match for cfind */
			break;

		if ((slash_pos = strrchr(path_buf, '/')) == NULL)
			break;

		path_start = path + (slash_pos - path_buf) + 1;

		if (slash_pos != path_buf)
			*slash_pos = '\0';
		else if (slash_pos == path_buf && *(slash_pos+1) != '\0')
			*(slash_pos+1) = '\0';
		else
			break;
	}
	if (CRASHDEBUG(2))
		error(INFO, "root_dentry:%lx path_start:%s\n",
			root, path_start);

	if (root && remaining_path)
		*remaining_path = path_start;

	FREEBUF(path_buf);
bail_out:
	return root;
}

static ulong
path_to_dentry(char *path, ulong *inode)
{
	int i, count;
	ulong *subdirs_list, root, d, dentry;
	char *path_buf, *dentry_buf, *slash_pos, *path_start, *name;

	root = get_mntpoint_dentry(path, &path_start);
	if (!root) {
		error(INFO, "%s: mount point not found\n", path);
		return 0;
	}

	path_buf = GETBUF(strlen(path_start) + 1);
	memcpy(path_buf, path_start, strlen(path_start) + 1);
	path_start = path_buf;

	dentry = 0;
	dentry_buf = GETBUF(SIZE(dentry));
	d = root;

	while (strlen(path_start)) {
		if ((slash_pos = strchr(path_start, '/')))
			*slash_pos = '\0';

		if (!(subdirs_list = get_subdirs_list(&count, d)))
			goto not_found;

		for (i = 0; i < count; i++) {
			d = subdirs_list[i];
			if (!readmem(d, KVADDR, dentry_buf, SIZE(dentry),
			    "dentry buffer", RETURN_ON_ERROR))
				continue;

			name = get_dentry_name(d, dentry_buf);

			if (CRASHDEBUG(2))
				error(INFO, "q:%s %3d: d:%lx name:%s\n",
					path_start, i, d, name);

			if (STREQ(path_start, name)) {
				if (slash_pos)
					break;
				else {
					FREEBUF(subdirs_list);
					goto found;
				}
			}
		}
		FREEBUF(subdirs_list);

		/* no such dentry */
		if (i == count)
			goto not_found;

		if (slash_pos)
			path_start = slash_pos + 1;
	}
	/* the path ends with '/' */
	if (!readmem(d, KVADDR, dentry_buf, SIZE(dentry),
	    "dentry buffer", RETURN_ON_ERROR))
		goto not_found;

found:
	dentry = d;
	if (inode)
		*inode = ULONG(dentry_buf + OFFSET(dentry_d_inode));

not_found:
	FREEBUF(dentry_buf);
	FREEBUF(path_buf);

	return dentry;
}

typedef struct {
	ulong dentry;
	char *name;
	uint i_mode;
} dentry_info_t;

static void
recursive_list_dir(char *arg, ulong pdentry, uint pi_mode)
{
	ulong *list;
	int i, count, nr_negdents = 0;
	char *slash;
	ulong d, inode;
	uint i_mode;
	dentry_info_t *dentry_list, *p;

	if (!(flags & FIND_COUNT_DENTRY))
		fprintf(fp, "%16lx %s\n", pdentry, arg);

	if (!S_ISDIR(pi_mode))
		return;

	if (!(list = get_subdirs_list(&count, pdentry)))
		return;

	slash = (strlen(arg) == 1) ? "" : "/";
	dentry_list = (dentry_info_t *)GETBUF(sizeof(dentry_info_t) * count);

	for (i = 0, p = dentry_list; i < count; i++) {
		d = list[i];
		readmem(d, KVADDR, dentry_data, SIZE(dentry),
			"dentry", FAULT_ON_ERROR);

		inode = ULONG(dentry_data + OFFSET(dentry_d_inode));
		if (inode && get_inode_info(inode, &i_mode, NULL, NULL, NULL))
			p->i_mode = i_mode;
		else {
			if (flags & FIND_COUNT_DENTRY) {
				nr_negdents++;
				continue;
			}
			if (!(flags & SHOW_INFO_NEG_DENTS))
				continue;
		}
		p->dentry = d;
		p->name = strdup(get_dentry_name(d, dentry_data));
		p++;
	}

	if (flags & FIND_COUNT_DENTRY) {
		fprintf(fp, count_dentry_fmt,
			count, count - nr_negdents, nr_negdents, arg);
		total_dentry += count;
		total_negdent += nr_negdents;
	}

	count = p - dentry_list;

	for (i = 0, p = dentry_list; i < count; i++, p++) {
		if (S_ISDIR(p->i_mode)) {
			char *path = GETBUF(BUFSIZE);
			snprintf(path, BUFSIZE, "%s%s%s", arg, slash, p->name);

			d = get_mntpoint_dentry(path, NULL);
			if (d) {
				readmem(d, KVADDR, dentry_data, SIZE(dentry),
					"dentry", FAULT_ON_ERROR);
				inode = ULONG(dentry_data + OFFSET(dentry_d_inode));
				if (inode && get_inode_info(inode, &i_mode,
						NULL, NULL, NULL))
					recursive_list_dir(path, d, i_mode);
				else
					error(INFO, "%s: invalid inode\n", path);
			} else /* normal directory */
				recursive_list_dir(path, p->dentry, p->i_mode);

			FREEBUF(path);
		} else if (!(flags & FIND_COUNT_DENTRY))
			fprintf(fp, "%16lx %s%s%s\n", p->dentry, arg, slash, p->name);

		free(p->name);
	}

	FREEBUF(dentry_list);
	FREEBUF(list);
}

/*
 * Currently just squeeze a series of slashes into a slash,
 * and remove the last slash.
 */
static void
normalize_path(char *path)
{
	char *s, *d;

	if (!path || *path == '\0')
		return;

	s = d = path;
	while (*s) {
		if (*s == '/' && *(s+1) == '/') {
			s++;
			continue;
		}
		*d++ = *s++;
	}
	*d = '\0';

	d--;
	if (d != path && *d == '/')
		*d = '\0';
}

static void
do_command(char *arg)
{
	ulong inode, i_mapping, root, dentry;
	struct list_pair lp;
	ulong count, nrpages;
	uint i_mode;

	inode = dentry = 0;
	if (flags & DUMP_CACHES)
		inode = htol(arg, RETURN_ON_ERROR|QUIET, NULL);

	if (flags & (SHOW_INFO|FIND_FILES) || inode == BADADDR) {
		if (arg[0] != '/')
			cmd_usage(pc->curcmd, SYNOPSIS);

		normalize_path(arg);

		dentry = path_to_dentry(arg, &inode);
		if (!dentry) {
			error(INFO, "%s: not found in dentry cache\n", arg);
			return;
		} else if (!inode) {
			error(INFO, "%s: negative dentry\n", arg);
			return;
		}
	}

	if (!get_inode_info(inode, &i_mode, &i_mapping, &i_size, &nrpages))
		return;

	if (flags & DUMP_CACHES) {
		if (!S_ISREG(i_mode)) {
			error(INFO, "%s: not regular file\n", arg);
			return;
		} else if (!nrpages) {
			error(INFO, "%s: no page caches\n", arg);
			return;
		}

		root = i_mapping + OFFSET(address_space_page_tree);
		lp.value = dump_slot;
		nr_written = nr_excluded = 0;

		pgbuf = GETBUF(PAGESIZE());

		if (MEMBER_EXISTS("address_space", "i_pages") &&
		    STREQ(MEMBER_TYPE_NAME("address_space", "i_pages"), "xarray"))
			count = do_xarray(root, XARRAY_DUMP_CB, &lp);
		else
			count = do_radix_tree(root, RADIX_TREE_DUMP_CB, &lp);

		FREEBUF(pgbuf);

		if (!(flags & DUMP_DONT_SEEK))
			ftruncate(fileno(outfp), i_size);

		if (nr_excluded)
			error(INFO, "%lu/%lu pages excluded\n",
				nr_excluded, count);
		if (CRASHDEBUG(1))
			error(INFO, "%lu/%lu pages written\n",
				nr_written, count);

	} else if (flags & SHOW_INFO) {
		fprintf(fp, header_fmt, "DENTRY", "INODE", "I_MAPPING",
			"NRPAGES", "%", "PATH");

		int pct = calc_cached_percent(nrpages, i_size);

		if (flags & SHOW_INFO_DIRS || !S_ISDIR(i_mode)) {
			fprintf(fp, dentry_fmt, dentry, inode, i_mapping,
				nrpages, pct, arg, get_type_indicator(i_mode));
			if (CRASHDEBUG(1))
				fprintf(fp, "  i_mode:%6o i_size:%llu (%llu)\n",
					i_mode, i_size, byte_to_page(i_size));
		} else {
			fprintf(fp, dentry_fmt, dentry, inode, i_mapping,
				nrpages, pct, ".", get_type_indicator(i_mode));
			if (CRASHDEBUG(1))
				fprintf(fp, "  i_mode:%6o i_size:%llu (%llu)\n",
					i_mode, i_size, byte_to_page(i_size));
			show_subdirs_info(dentry);
		}
	} else if (flags & FIND_FILES) {
		if (flags & FIND_COUNT_DENTRY) {
			fprintf(fp, count_header_fmt,
				"TOTAL", "DENTRY", "N_DENT", "PATH");
			total_dentry = total_negdent = 0;
		}

		recursive_list_dir(arg, dentry, i_mode);

		if (flags & FIND_COUNT_DENTRY) {
			fprintf(fp, count_dentry_fmt,
				total_dentry, total_dentry - total_negdent,
				total_negdent, "TOTAL");
		}
	}
}

static void
init_cache(void) {
	/* In case that the last command was interrupted. */
	if (mount_data) {
		mount_data = NULL;
		mount_path = NULL;
		mount_count = 0;
	}
	dentry_data = GETBUF(SIZE(dentry));
}

static void
clear_cache(void)
{
	int i;

	if (mount_data) {
		FREEBUF(mount_data);
		for (i = 0; i < mount_count; i++) {
			FREEBUF(mount_path[i]);
		}
		FREEBUF(mount_path);
		mount_data = NULL;
		mount_path = NULL;
		mount_count = 0;
	}
	FREEBUF(dentry_data);
}

static void
set_default_task_context(void)
{
	ulong pid = 0;

	while ((tc = pid_to_context(pid)) == NULL)
		pid++;
}

void
cmd_ccat(void)
{
	int c;
	char *arg;
	ulong value;

	flags = DUMP_CACHES;
	tc = NULL;

	while ((c = getopt(argcnt, args, "n:S")) != EOF) {
		switch(c) {
		case 'n':
			switch (str_to_context(optarg, &value, &tc)) {
			case STR_PID:
			case STR_TASK:
				break;
			case STR_INVALID:
				error(FATAL, "invalid task or pid value: %s\n",
					optarg);
				break;
			}
			break;
		case 'S':
			flags |= DUMP_DONT_SEEK;
			break;
		default:
			argerrs++;
			break;
		}
	}

	if (argerrs || !args[optind])
		cmd_usage(pc->curcmd, SYNOPSIS);

	arg = args[optind++];

	if (args[optind]) {
		if (access(args[optind], F_OK) == 0) {
			error(INFO, "%s: %s\n",
				args[optind], strerror(EEXIST));
			return;
		}
		if ((outfp = fopen(args[optind], "w")) == NULL) {
			error(INFO, "cannot open %s: %s\n",
				args[optind], strerror(errno));
			return;
		}
		set_tmpfile2(outfp);
	} else
		outfp = fp;

	if (!tc)
		set_default_task_context();

	init_cache();

	do_command(arg);

	clear_cache();
	if (outfp != fp)
		close_tmpfile2();
}

char *help_ccat[] = {
"ccat",				/* command name */
"dump page caches",		/* short description */
"[-S] [-n pid|task] inode|abspath [outfile]",
				/* argument synopsis, or " " if none */
"  This command dumps the page caches of a specified inode or path like",
"  \"cat\" command.",
"",
"       -S  do not fseek() and ftruncate() to outfile in order to",
"           create a non-sparse file.",
"    inode  a hexadecimal inode pointer.",
"  abspath  an absolute path.",
"  outfile  a file path to be written. If a file already exists there,",
"           the command fails.",
"",
"  For kernels supporting mount namespaces, the -n option may be used to",
"  specify a task that has the target namespace:",
"",
"    -n pid   a process PID.",
"    -n task  a hexadecimal task_struct pointer.",
"",
"EXAMPLE",
"  Dump the existing page caches of the \"/var/log/messages\" file:",
"",
"    %s> ccat /var/log/messages",
"    Sep 16 03:13:01 host systemd: Started Session 559694 of user root.",
"    Sep 16 03:13:01 host systemd: Starting Session 559694 of user root.",
"    Sep 16 03:13:39 host dnsmasq-dhcp[24341]: DHCPREQUEST(virbr0) 192.168",
"    Sep 16 03:13:39 host dnsmasq-dhcp[24341]: DHCPACK(virbr0) 192.168.122",
"    ...",
"",
"  Restore the size and data offset of the \"messages\" file as well to the",
"  \"messages.sparse\" file even if some of its page caches don't exist, so",
"  it could become sparse:",
"",
"    %s> ccat /var/log/messages messages.sparse",
"",
"  Create the non-sparse \"messages.non-sparse\" file:",
"",
"    %s> ccat -S /var/log/messages messages.non-sparse",
"",
"  NOTE: Redirecting to a file will also works, but it can includes crash's",
"  messages, so specifying an outfile is recommended for restoring a file.",
NULL
};

void
cmd_cls(void)
{
	int c;
	ulong value;

	flags = SHOW_INFO;
	tc = NULL;

	while ((c = getopt(argcnt, args, "adn:U")) != EOF) {
		switch(c) {
		case 'a':
			flags |= SHOW_INFO_NEG_DENTS;
			break;
		case 'd':
			flags |= SHOW_INFO_DIRS;
			break;
		case 'n':
			switch (str_to_context(optarg, &value, &tc)) {
			case STR_PID:
			case STR_TASK:
				break;
			case STR_INVALID:
				error(FATAL, "invalid task or pid value: %s\n",
					optarg);
				break;
			}
			break;
		case 'U':
			flags |= SHOW_INFO_DONT_SORT;
			break;
		default:
			argerrs++;
			break;
		}
	}

	if (argerrs || !args[optind])
		cmd_usage(pc->curcmd, SYNOPSIS);

	if (!tc)
		set_default_task_context();

	init_cache();

	do_command(args[optind++]);

	while (args[optind]) {
		fprintf(fp, "\n");
		do_command(args[optind++]);
	}

	clear_cache();
}

char *help_cls[] = {
"cls",				/* command name */
"list dentry and inode caches",	/* short description */
"[-adU] [-n pid|task] abspath...",	/* argument synopsis, or " " if none */

"  This command displays the addresses of dentry, inode and i_mapping,",
"  and nrpages of a specified absolute path and its subdirs if it exists",
"  in dentry cache.",
"",
"    -a  also display negative dentries in the subdirs list.",
"    -d  display the directory itself only, without its contents.",
"    -U  do not sort, list dentries in directory order.",
"",
"  For kernels supporting mount namespaces, the -n option may be used to",
"  specify a task that has the target namespace:",
"",
"    -n pid   a process PID.",
"    -n task  a hexadecimal task_struct pointer.",
"",
"EXAMPLE",
"  Display the \"/var/log/messages\" regular file's information:",
"",
"    %s> cls /var/log/messages",
"    DENTRY           INODE            I_MAPPING        NRPAGES   % PATH",
"    ffff9c0c28fda480 ffff9c0c22c675b8 ffff9c0c22c67728     220 100 /var/log/messages",
"",
"  The '\%' column shows the percentage of cached pages in the file.",
"",
"  Display the \"/var/log\" directory and its subdirs information:",
"",
"    %s> cls /var/log",
"    DENTRY           INODE            I_MAPPING        NRPAGES   % PATH",
"    ffff9c0c3eabe300 ffff9c0c3e875b78 ffff9c0c3e875ce8       0   0 ./",
"    ffff9c0c16a22900 ffff9c0c16ada2f8 ffff9c0c16ada468       0   0 anaconda/",
"    ffff9c0c37611000 ffff9c0c3759f5b8 ffff9c0c3759f728       0   0 audit/",
"    ffff9c0c375ccc00 ffff9c0c3761c8b8 ffff9c0c3761ca28       1 100 btmp",
"    ffff9c0c28fda240 ffff9c0c22c713f8 ffff9c0c22c71568       6 100 cron",
"    ffff9c0c3eb7f180 ffff9c0bfd402a78 ffff9c0bfd402be8      36   7 dnf.librepo.log",
"    ...",
"",
"  Display the \"/var/log\" directory itself only:",
"",
"    %s> cls -d /var/log",
"    DENTRY           INODE            I_MAPPING        NRPAGES   % PATH",
"    ffff9c0c3eabe300 ffff9c0c3e875b78 ffff9c0c3e875ce8       0   0 /var/log/",
NULL
};

void
cmd_cfind(void)
{
	int c;
	ulong value;

	flags = FIND_FILES;
	tc = NULL;

	while ((c = getopt(argcnt, args, "acn:")) != EOF) {
		switch(c) {
		case 'a':
			flags |= SHOW_INFO_NEG_DENTS;
			break;
		case 'c':
			flags |= FIND_COUNT_DENTRY;
			break;
		case 'n':
			switch (str_to_context(optarg, &value, &tc)) {
			case STR_PID:
			case STR_TASK:
				break;
			case STR_INVALID:
				error(FATAL, "invalid task or pid value: %s\n",
					optarg);
				break;
			}
			break;
		default:
			argerrs++;
			break;
		}
	}

	if (argerrs || !args[optind])
		cmd_usage(pc->curcmd, SYNOPSIS);

	if (!tc)
		set_default_task_context();

	init_cache();

	do_command(args[optind]);

	clear_cache();
}

char *help_cfind[] = {
"cfind",
"search for files/dentries in a directory hierarchy",
"[-ac] [-n pid|task] abspath",

"  This command searches for files/dentries in a directory hierarchy across",
"  mounted file systems like \"find\" command.",
"",
"    -a  also display negative dentries.",
"    -c  count dentries in each directory.",
"",
"  For kernels supporting mount namespaces, the -n option may be used to",
"  specify a task that has the target namespace:",
"",
"    -n pid   a process PID.",
"    -n task  a hexadecimal task_struct pointer.",
"",
"EXAMPLE",
"  Search for \"messages\" file in / hierarchy:",
"",
"    %s> cfind / | grep messages",
"    ffff88010113be00 /var/log/messages",
"    ffff880449f86b40 /usr/lib/python2.7/site-packages/babel/messages",
"",
"  Count dentries in /tmp directory and its subdirectories:",
"",
"    %s> cfind -c /tmp",
"      TOTAL DENTRY N_DENT PATH",
"        615      9    606 /tmp",
"          1      1      0 /tmp/systemd-private-f94cc7530e524709...-U8nOww",
"          1      1      0 /tmp/systemd-private-f94cc7530e524709...-qb8Qke",
"          1      1      0 /tmp/systemd-private-f94cc7530e524709...-aVh468",
"        618     12    606 TOTAL",
NULL
};

static struct command_table_entry command_table[] = {
	{ "ccat", cmd_ccat, help_ccat, 0},
	{ "cls", cmd_cls, help_cls, 0},
	{ "cfind", cmd_cfind, help_cfind, 0},
	{ NULL },
};

#define DL_EXCLUDE_CACHE_PRI	(0x04)

void __attribute__((constructor))
cacheutils_init(void)
{
	int dump_level;

	register_extension(command_table);

	CU_OFFSET_INIT(inode_i_size, "inode", "i_size");
	CU_OFFSET_INIT(vfsmount_mnt_root, "vfsmount", "mnt_root");
	CU_OFFSET_INIT(dentry_d_subdirs, "dentry", "d_subdirs");
	CU_OFFSET_INIT(dentry_d_child, "dentry", "d_child");
	if (CU_INVALID_MEMBER(dentry_d_child))	/* RHEL7 and older */
		CU_OFFSET_INIT(dentry_d_child, "dentry", "d_u");

	if (CRASHDEBUG(1)) {
		error(INFO, "       inode_i_size: %lu\n",
			CU_OFFSET(inode_i_size));
		error(INFO, "  vfsmount_mnt_root: %lu\n",
			CU_OFFSET(vfsmount_mnt_root));
		error(INFO, "   dentry_d_subdirs: %lu\n",
			CU_OFFSET(dentry_d_subdirs));
		error(INFO, "     dentry_d_child: %lu\n",
			CU_OFFSET(dentry_d_child));
	}

	if ((*diskdump_flags & KDUMP_CMPRS_LOCAL) &&
	    ((dump_level = get_dump_level()) >= 0) &&
	    (dump_level & DL_EXCLUDE_CACHE_PRI))
		error(WARNING, "\"ccat\" command is unusable because all of"
			" cache pages are excluded (dump_level:%d)\n",
			dump_level);
}

void __attribute__((destructor))
cacheutils_fini(void)
{
}
