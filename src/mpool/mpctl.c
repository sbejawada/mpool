// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

#define _GNU_SOURCE

#include <util/platform.h>
#include <util/alloc.h>
#include <util/string.h>
#include <util/minmax.h>
#include <util/parser.h>
#include <util/valgrind.h>
#include <util/printbuf.h>
#include <util/page.h>

#include <mpctl/impool.h>
#include <mpctl/imlog.h>
#include <mpctl/imdc.h>

#include "discover.h"

#include <mpctl/impool.h>
#include <mpool/mpool.h>

#include "dev_cntlr.h"
#include "device_table.h"
#include <mpcore/mpcore_defs.h>

#include "logging.h"

#include <libgen.h>
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <ftw.h>

#include "device_table.h"

/*
 * This is the userland metadata for mcachefs maps.
 */
struct mpool_mcache_map {
	size_t  mh_bktsz;       /* mcache map file bucket size */
	void   *mh_addr;        /* mcache map file base mmap addr if mmapped */
	int     mh_mbidc;       /* number of mblock IDs in mcache map file */
	int     mh_fd;
	off_t   mh_offset;
	size_t  mh_len;
};

struct devrpt_tab {
	enum mpool_rc   rcode;
	const char     *msg;
};

static const struct devrpt_tab devrpt_tab[] = {
	{ MPOOL_RC_NONE,    "Success" },

	{ MPOOL_RC_OPEN,    "Unable to open" },
	{ MPOOL_RC_PARM,    "Cannot query or set parms or parms invalid" },
	{ MPOOL_RC_MAGIC,   "Valid magic found on device" },
	{ MPOOL_RC_STAT,    "Device state does not permit operation" },
	{ MPOOL_RC_ENOMEM,  "No system memory available" },

	{ MPCTL_RC_DEVRW,          "Unable to read/write device" },
	{ MPCTL_RC_NOTACTIVATED,   "mpool is not activated" },
	{ MPCTL_RC_DEVACTIVATED,   "The device belongs to a activated mpool" },
	{ MPCTL_RC_MP_NODEV,       "No such mpool" },
	{ MPCTL_RC_INVALDEV,       "Unable to add device" },
	{ MPCTL_RC_MPEXIST,        "mpool already exists" },
	{ MPCTL_RC_ENTNAM_INV,     "Invalid name or label" },

	{ 0, NULL }
};

const char *mpool_devrpt_strerror(enum mpool_rc rcode)
{
	const struct devrpt_tab *entry = devrpt_tab;

	while (entry->msg && entry->rcode != rcode)
		++entry;

	return entry->msg ?: "Invalid rcode";
}

/* mpool_transmogrify() - transmogrify a vector of entries
 *
 * @dpaths:   vector of pointers to contiguously packed strings
 * @entry:    a tuple of (mp_name, uuid, drive_path)
 * @sep:      the separator to place between contiguous entries in the dpaths
 * @dcnt:     the number of valid entries in the entry array
 *
 * dpaths is a vector of pointers followed by its payload of strings. If sep is
 * non-NUL, the first element points to a single string containing all paths
 * separated by sep.  The last element is always NUL-terminated.
 *
 * Returns: merr_t
 */
static merr_t mpool_transmogrify(char ***dpaths, struct imp_entry *entry, char sep, int dcnt)
{
	char   **dv;
	char    *p;
	int      i, j;

	dv = calloc(dcnt, (sizeof(char *) + sizeof(entry->mp_path) + 8));
	if (!dv)
		return merr(ENOMEM);

	p = (char *)&dv[dcnt];
	for (i = 0; i < dcnt; i++) {
		strcpy(p, entry[i].mp_path);
		j = strlen(p);
		p[j] = sep;
		dv[i] = p;
		p += j + 1;
	}

	*(--p) = 0; /* NUL terminate last entry */
	*dpaths = dv;

	return 0;
}

static void mpool_params_init2(struct mpool_params *dst, const struct mpool_params *src)
{
	mpool_params_init(dst);

	if (src)
		*dst = *src;
}

static mpool_err_t mpool_ioctl(int fd, int cmd, void *arg)
{
	struct mpioc_cmn   *cmn = arg;
	int                 rc;

	cmn->mc_merr_base = mpool_merr_base;

	rc = ioctl(fd, cmd, arg);

	return rc ? merr(errno) : cmn->mc_err;
}

/**
 * mpool_ugm_check() - Check and set device user/group/mode
 * @name:       mpool name (may be null, preferred to %fd)
 * @fd:         file descriptor of mpool special device
 * @params:     mpool parameters
 *
 * systemd-udev by default will not chown a uid/gid that it
 * cannot resolve to a valid uid/gid, nor will it chmod a
 * file to zero.  This function is called after a perms
 * change was successfully applied to the mpool module.  It
 * checks to see if the change was applied.  If not it
 * attempts to make the change directly to the special file,
 * hence it also acts as a backstop for when udevd isn't
 * getting the job done.
 */
static mpool_err_t mpool_ugm_check(const char *name, int fd, const struct mpool_params *params)
{
	struct mpool   *mp = NULL;
	struct stat     sb;

	mode_t  mode = params->mp_mode;
	uid_t   uid = params->mp_uid;
	gid_t   gid = params->mp_gid;
	merr_t  err = 0;
	int     rc, i;

	if (mode == -1 && uid == -1 && gid == -1)
		return 0;

	if (mode != -1)
		mode &= 0777;

	if (name) {
		err = mpool_open(name, O_RDWR, &mp, NULL);
		if (err)
			return err;

		fd = mp->mp_fd;
	}

	for (rc = i = 0; i < 15; ++i) {
		usleep(10000 * i + 1000);

		rc = fstat(fd, &sb);
		if (!rc && (uid == -1 || sb.st_uid == params->mp_uid) &&
		    (gid == -1 || sb.st_gid == params->mp_gid) &&
		    (mode == -1 || (sb.st_mode & 0777) == mode))
			goto errout;
	}

	if (rc) {
		err = merr(rc);
		goto errout;
	}

	if (uid != -1 && sb.st_uid != uid)
		if (fchown(fd, uid, -1))
			err = merr(errno);

	if (gid != -1 && sb.st_gid != gid)
		if (fchown(fd, -1, gid))
			err = merr(errno);

	if (mode != -1 && (sb.st_mode & 0777) != mode)
		if (fchmod(fd, mode))
			err = merr(errno);

errout:
	mpool_close(mp);

	return err;
}

merr_t mp_list_mpool_by_device(int devicec, char **devicev, char *buf, size_t buf_len)
{
	struct imp_entry   *mpool[MPOOL_COUNT_MAX];
	struct imp_entry   *entry = NULL;

	merr_t      err = 0;
	int         i;
	int         entry_cnt = 0;
	int         mpool_cnt = 0;
	u32         flags = 0;
	size_t      buf_offset = 0;
	const char *comma = "";

	err = imp_entries_get(NULL, NULL, NULL, &flags, &entry,	&entry_cnt);
	if (err)
		goto exit;

	for (i = 0; i < entry_cnt; i++) {
		int  k, j;
		bool dup;

		for (k = 0; k < devicec; k++)
			if (!strcmp(entry[i].mp_path, devicev[k]))
				break;

		if (k >= devicec)
			continue;

		/* The k-th device in devicev is part of an mpool. This mpool
		 * needs to be added to buf iff it is not a dup
		 */
		dup = false;
		for (j = 0; j < mpool_cnt; j++) {
			if (!strcmp(entry[i].mp_name, mpool[j]->mp_name)) {
				dup = true;
				break;
			}
		}

		if (dup == false) {
			mpool[mpool_cnt] = &entry[i];

			if (buf_len - buf_offset < strlen(mpool[mpool_cnt]->mp_name) + 1) {
				err = merr(ENOBUFS);
				goto exit;
			}

			snprintf_append(buf, buf_len, &buf_offset, "%s%s", comma,
					mpool[mpool_cnt]->mp_name);
			comma = ", ";
			mpool_cnt++;
		}
	}
exit:
	free(entry);

	return err;
}

mpool_err_t
mp_sb_erase(int devicec, char **devicev, struct mpool_devrpt *devrpt, char *pools, size_t pools_len)
{
	struct pd_prop   *pd_prop = NULL;
	merr_t            err;

	mpool_devrpt_init(devrpt);

	if (!devicev || !pools || pools_len < 1 || !devrpt ||
	    devicec < 1 || devicec > MPOOL_DRIVES_MAX)
		return merr(EINVAL);

	err = mp_list_mpool_by_device(devicec, devicev, pools, pools_len);
	if (err)
		goto exit;

	/* Get pd properties and use them for sb_erase() */
	err = imp_dev_alloc_get_prop(devicec, devicev, &pd_prop);
	if (err)
		goto exit;

	err = mpool_sb_erase(devicec, devicev, pd_prop, devrpt);
	if (err)
		goto exit;

exit:
	if (pd_prop)
		free(pd_prop);

	return err;
}

/**
 * mpool_strchk() - check if "name" is an alphanumeric string
 *	that doesn't contain '-', '_' nor '.'
 * @name: zero terminated string
 * @maxlen: maximum length of "name" (not counting the trailing zero).
 */
static merr_t mpool_strchk(const char *str, size_t minlen, size_t maxlen, struct mpool_devrpt *ei)
{
	if (!str || strnlen(str, minlen) < minlen)
		return merr(EINVAL);

	/* Don't allow hyphen as first character */
	if (*str == '-') {
		mpool_devrpt(ei, MPCTL_RC_ENTNAM_INV, -1, str);
		return merr(EINVAL);
	}

	/* Check that str contains only characters from the
	 * Portable Filename Character Set [-_.A-Za-z0-9].
	 */
	while (*str && maxlen-- > 0) {
		if (!isalnum(*str) && !strchr("._-", *str)) {
			mpool_devrpt(ei, MPCTL_RC_ENTNAM_INV, -1, str);
			return merr(EINVAL);
		}

		++str;
	}

	/* If length of str > maxlen, return ENAMETOOLONG */
	if (*str) {
		mpool_devrpt(ei, MPCTL_RC_ENTNAM_INV, -1, str);
		return merr(ENAMETOOLONG);
	}

	return 0;
}

static merr_t
discover(
	const char         *name,
	u32                *flags,
	struct imp_entry  **entry,
	int                *dcnt,
	char             ***dpaths,
	char                sep,
	const char         *prefix)
{
	struct mpool_uuid   uuid;

	merr_t  err;
	int     rc;

	/* Is the passed-in name an mpool name or uuid? */
	rc = mpool_parse_uuid(name, &uuid);
	if (rc) {
		/* This is a name, so validate it */
		err = mpool_strchk(name, 1, MPOOL_NAMESZ_MAX - 1, NULL);
		if (err)
			return err;
	}

	err = imp_entries_get(rc ? name : NULL, rc ? NULL : &uuid, NULL, flags, entry, dcnt);

	if (*dcnt > MPOOL_DRIVES_MAX) {
		free(*entry);
		return merr(E2BIG);
	}

	if (!err && *dcnt) {
		err = mpool_transmogrify(dpaths, *entry, sep, *dcnt);
		if (err)
			free(*entry);
	} else if (!*dcnt) {
		err = merr(ENOENT);
	}

	return err;
}

static void mpool_rundir_create(const char *mpname)
{
	struct mpool_params params;
	struct mpool       *mp;

	char    path[PATH_MAX];
	char    errbuf[128];
	merr_t  err;
	int     rc;

	if (!mpname)
		return;

	err = mpool_open(mpname, 0, &mp, NULL);
	if (err) {
		fprintf(stderr, "%s: mp_open(%s): %s\n", __func__, mpname,
			mpool_strerror(err, errbuf, sizeof(errbuf)));
		return;
	}

	err = mpool_params_get(mp, &params, NULL);

	mpool_close(mp);

	if (err) {
		fprintf(stderr, "%s: mpool_params_get(%s): %s", __func__, mpname,
			mpool_strerror(err, errbuf, sizeof(errbuf)));
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", MPOOL_RUNDIR_ROOT, mpname);

	params.mp_mode |= (params.mp_mode & 0700) ? 0100 : 0;
	params.mp_mode |= (params.mp_mode & 0070) ? 0010 : 0;
	params.mp_mode |= (params.mp_mode & 0007) ? 0001 : 0;
	params.mp_mode &= 0777;

	rc = mkdir(path, params.mp_mode);
	if (rc && errno != EEXIST) {
		err = merr(errno);
		fprintf(stderr, "%s: mkdir(%s, %04o): %s\n", __func__, path, params.mp_mode,
			mpool_strerror(err, errbuf, sizeof(errbuf)));
		return;
	}

	rc = chown(path, params.mp_uid, params.mp_gid);
	if (rc) {
		err = merr(errno);
		fprintf(stderr, "%s: chown(%s, %u, %u): %s\n", __func__, path, params.mp_uid,
			params.mp_gid, mpool_strerror(err, errbuf, sizeof(errbuf)));
		remove(path);
	}
}

static int
mpool_rundir_destroy_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	return remove(fpath);
}

static void mpool_rundir_destroy(const char *mpname)
{
	char   path[PATH_MAX];

	if (!mpname)
		return;

	snprintf(path, sizeof(path), "%s/%s", MPOOL_RUNDIR_ROOT, mpname);

	nftw(path, mpool_rundir_destroy_cb, 4, FTW_DEPTH | FTW_PHYS);
}

mpool_err_t
mpool_mclass_add(
	const char             *mpname,
	const char             *devname,
	enum mp_media_classp    mclassp,
	struct mpool_params    *params,
	uint32_t                flags,
	struct mpool_devrpt    *ei)
{
	struct pd_prop      pd_prop;
	struct mpioc_drive  drv;
	struct mpool       *mp;
	struct mpool_params aparams;

	char    rpath[PATH_MAX];
	bool    is_activated;
	merr_t  err;
	u64     mbsz;

	if (!mpname || !devname)
		return merr(EINVAL);

	mpool_devrpt_init(ei);

	is_activated = imp_mpool_activated(mpname);
	if (!is_activated) {
		mpool_devrpt(ei, MPCTL_RC_NOTACTIVATED, -1, mpname);
		return merr(EINVAL);
	}

	if (imp_device_allocated(devname, flags)) {
		mpool_devrpt(ei, MPOOL_RC_MAGIC, -1, devname);
		err = merr(EBUSY);
		goto out;
	}

	err = imp_dev_get_prop(devname, &pd_prop);
	if (err) {
		mpool_devrpt(ei, MPCTL_RC_DEVRW, -1, devname);
		mpool_elog(MPOOL_ERR "mpool %s create, unable to get device %s properties @@e",
			   err, mpname, devname);
		goto out;
	}
	pd_prop.pdp_mclassp = mclassp;

	if (!realpath(devname, rpath))
		return merr(errno);

	if (!params) {
		mpool_params_init(&aparams);
		params = &aparams;
	}

	mbsz = params->mp_mblocksz[mclassp];
	mbsz = mbsz ? : MPOOL_MBSIZE_MB_DEFAULT;
	pd_prop.pdp_zparam.dvb_zonepg = ((mbsz << 20) >> PAGE_SHIFT);
	pd_prop.pdp_zparam.dvb_zonetot = pd_prop.pdp_devsz /
		(pd_prop.pdp_zparam.dvb_zonepg << PAGE_SHIFT);
	params->mp_mblocksz[mclassp] = mbsz;

	memset(&drv, 0, sizeof(drv));
	drv.drv_flags = flags;

	drv.drv_pd_prop = &pd_prop;
	drv.drv_dpathc = 1;
	drv.drv_dpaths = rpath;
	drv.drv_dpathssz = strlen(rpath) + 1; /* trailing NUL */

	err = mpool_open(mpname, O_RDWR | O_EXCL, &mp, ei);
	if (err)
		return err;

	err = mpool_ioctl(mp->mp_fd, MPIOC_DRV_ADD, &drv);

out:
	mpool_close(mp);

	return err;
}

mpool_err_t
mpool_mclass_get(struct mpool *mp, enum mp_media_classp mclass, struct mpool_mclass_props *props)
{
	struct mpioc_prop   mp_prop = { };
	struct mpioc_list   ls = {
		.ls_listv = &mp_prop,
		.ls_listc = 1,
		.ls_cmd = MPIOC_LIST_CMD_PROP_GET,
	};
	struct mpool_mclass_xprops *xprops;
	struct mpool_usage         *usage;

	merr_t  err;
	int     i;

	if (!mp || mclass >= MP_MED_NUMBER)
		return merr(EINVAL);

	err = mpool_ioctl(mp->mp_fd, MPIOC_PROP_GET, &ls);
	if (err)
		return err;

	xprops = NULL;
	for (i = 0; i < mp_prop.pr_mcxc; i++) {
		if (mp_prop.pr_mcxv[i].mc_mclass == mclass) {
			xprops = &mp_prop.pr_mcxv[i];
			break;
		}
	}

	if (!xprops)
		return merr(ENOENT);

	if (props) {
		props->mc_mblocksz = (xprops->mc_zonepg << PAGE_SHIFT) >> 20;

		usage = &xprops->mc_usage;
		props->mc_total = usage->mpu_usable + usage->mpu_spare;
		props->mc_usable = usage->mpu_usable;
		props->mc_used = usage->mpu_used;
		props->mc_spare = usage->mpu_spare;
		props->mc_spare_used = usage->mpu_spare - usage->mpu_fspare;
	}

	return 0;
}

mpool_err_t
mpool_create(
	const char             *mpname,
	const char             *devname,
	struct mpool_params    *params,
	uint32_t                flags,
	struct mpool_devrpt    *ei)
{
	struct mpioc_mpool  mp = { };
	struct pd_prop      pd_prop = { };

	char    rpath[PATH_MAX];
	merr_t  err;
	int     fd;
	u64     mbsz;
	u32     mdc0cap;
	u32     mdcncap;

	mpool_devrpt_init(ei);

	if (!mpname || !devname)
		return merr(EINVAL);

	err = mpool_strchk(mpname, 1, MPOOL_NAMESZ_MAX - 1, ei);
	if (err)
		return err;

	mpool_params_init2(&mp.mp_params, params);

	err = mpool_strchk(mp.mp_params.mp_label, 0, MPOOL_LABELSZ_MAX - 1, ei);
	if (err)
		return err;

	/* Check if this mpool or these drives already exist in an mpool */
	if (imp_mpool_exists(mpname, flags, NULL)) {
		mpool_devrpt(ei, MPCTL_RC_MPEXIST, -1, mpname);
		return merr(EEXIST);
	}

	if (imp_device_allocated(devname, flags)) {
		mpool_devrpt(ei, MPOOL_RC_MAGIC, -1, devname);
		return merr(EBUSY);
	}

	err = imp_dev_get_prop(devname, &pd_prop);
	if (err) {
		mpool_devrpt(ei, MPCTL_RC_DEVRW, -1, devname);
		return err;
	}

	if (!realpath(devname, rpath))
		return merr(errno);

	strlcpy(mp.mp_params.mp_name, mpname, sizeof(mp.mp_params.mp_name));

	mbsz = mp.mp_params.mp_mblocksz[MP_MED_CAPACITY];
	mbsz = mbsz ?: MPOOL_MBSIZE_MB_DEFAULT;
	mp.mp_params.mp_mblocksz[MP_MED_CAPACITY] = mbsz;

	pd_prop.pdp_mclassp = MP_MED_CAPACITY;
	pd_prop.pdp_zparam.dvb_zonepg = ((mbsz << 20) >> PAGE_SHIFT);
	pd_prop.pdp_zparam.dvb_zonetot = pd_prop.pdp_devsz /
		(pd_prop.pdp_zparam.dvb_zonepg << PAGE_SHIFT);

	mdc0cap = mp.mp_params.mp_mdc0cap;
	if (mdc0cap != 0 && mdc0cap < mbsz)
		mp.mp_params.mp_mdc0cap = mbsz;

	mdcncap = mp.mp_params.mp_mdcncap;
	if (mdcncap != 0 && mdcncap < mbsz)
		mp.mp_params.mp_mdcncap = mbsz;

	mp.mp_pd_prop = &pd_prop;
	mp.mp_flags   = flags;

	mp.mp_dpathc = 1;
	mp.mp_dpaths = rpath;
	mp.mp_dpathssz = strlen(rpath) + 1; /* trailing NUL */

	fd = open(MPC_DEV_CTLPATH, O_RDWR | O_CLOEXEC);
	if (-1 == fd) {
		err = merr(errno);
		mpool_devrpt(ei, MPOOL_RC_OPEN, -1, MPC_DEV_CTLPATH);
		return err;
	}

	err = mpool_ioctl(fd, MPIOC_MP_CREATE, &mp);
	if (!err) {
		if (!params || params->mp_mode != -1 ||
		    params->mp_uid != -1 || params->mp_gid != -1)
			err = mpool_ugm_check(mpname, -1, &mp.mp_params);

		if (params)
			*params = mp.mp_params;
	}

	if (!err)
		mpool_rundir_create(mpname);

	close(fd);

	return err;
}

mpool_err_t mpool_destroy(const char *mpname, uint32_t flags, struct mpool_devrpt *ei)
{
	struct imp_entry   *entries;
	struct mpioc_mpool  mp = { };

	char  **dpathv = NULL;
	merr_t  err;
	int     dcnt;
	int     fd;

	mpool_devrpt_init(ei);

	if (!mpname)
		return merr(EINVAL);

	fd = open(MPC_DEV_CTLPATH, O_RDWR | O_CLOEXEC);
	if (-1 == fd) {
		err = merr(errno);
		mpool_devrpt(ei, MPOOL_RC_OPEN, -1, MPC_DEV_CTLPATH);
		return err;
	}

	err = discover(mpname, &flags, &entries, &dcnt, &dpathv, '\n', __func__);
	if (err) {
		if (merr_errno(err) == ENOENT)
			mpool_devrpt(ei, MPCTL_RC_MP_NODEV, -1, mpname);
		close(fd);
		return err;
	}

	mpool_rundir_destroy(entries->mp_name);

	strlcpy(mp.mp_params.mp_name, entries->mp_name, sizeof(mp.mp_params.mp_name));

	mp.mp_pd_prop = imp_entries2pd_prop(dcnt, entries);
	if (!mp.mp_pd_prop) {
		mpool_devrpt(ei, MPOOL_RC_ENOMEM, -1, "imp_entries2pd_prop");
		err = merr(ENOMEM);
		goto errout;
	}

	mp.mp_dpathc = dcnt;
	mp.mp_dpaths = dpathv[0];
	mp.mp_dpathssz = strlen(dpathv[0]) + 1; /* trailing NUL */
	mp.mp_flags = flags;

	err = mpool_ioctl(fd, MPIOC_MP_DESTROY, &mp);

errout:
	free(mp.mp_pd_prop);
	free(entries);
	free(dpathv);
	close(fd);

	return err;
}

mpool_err_t mpool_list(int *propscp, struct mpool_params **propsvp, struct mpool_devrpt *ei)
{
	struct mpioc_prop      *propv;
	struct mpioc_list       ls;
	struct mpool_params    *dst;
	size_t                  propmax;
	merr_t                  err;
	int                     fd, i;

	mpool_devrpt_init(ei);

	if (!propscp || !propsvp)
		return merr(EINVAL);

	propmax = 1024; /* get from sysfs */
	*propsvp = NULL;
	*propscp = 0;

	propv = calloc(propmax, sizeof(*propv));
	if (!propv)
		return merr(ENOMEM);

	memset(&ls, 0, sizeof(ls));
	ls.ls_cmd = MPIOC_LIST_CMD_PROP_LIST;
	ls.ls_listc = propmax;
	ls.ls_listv = propv;

	fd = open(MPC_DEV_CTLPATH, O_RDONLY | O_CLOEXEC);
	if (-1 == fd) {
		err = merr(errno);
		mpool_devrpt(ei, MPOOL_RC_OPEN, -1, MPC_DEV_CTLPATH);
		free(propv);
		return err;
	}

	err = mpool_ioctl(fd, MPIOC_PROP_GET, &ls);
	if (err) {
		free(propv);
		close(fd);
		return err;
	}

	close(fd);

	dst = (void *)propv;

	*propscp = ls.ls_listc;
	*propsvp = dst;

	for (i = 0; i < ls.ls_listc; ++i)
		memmove(dst + i, &propv[i].pr_xprops.ppx_params, sizeof(*dst));

	return 0;
}

mpool_err_t mpool_scan(int *propscp, struct mpool_params **propsvp, struct mpool_devrpt *ei)
{
	struct imp_entry       *entryv = NULL;
	struct mpool_params    *propsv, *props;

	int     entryc = 0, mpool_cnt = 0;
	int     i, j;
	merr_t  err;

	if (!propscp || !propsvp)
		return merr(EINVAL);

	*propsvp = NULL;
	*propscp = 0;

	err = imp_entries_get(NULL, NULL, NULL, NULL, &entryv, &entryc);
	if (err || entryc == 0)
		return err;

	propsv = calloc(entryc, sizeof(*propsv));
	if (!propsv) {
		free(entryv);
		return merr(ENOMEM);
	}

	for (i = 0; i < entryc; i++) {
		bool   dup = false;

		for (j = 0; j < mpool_cnt && !dup; ++j)
			dup = !strcmp(entryv[i].mp_name, propsv[j].mp_name);

		if (dup)
			continue;

		props = propsv + mpool_cnt++;
		strlcpy(props->mp_name, entryv[i].mp_name, sizeof(props->mp_name));
		memcpy(&props->mp_poolid, &entryv[i].mp_uuid, MPOOL_UUID_SIZE);
	}

	*propscp = mpool_cnt;
	*propsvp = propsv;

	free(entryv);

	return 0;
}

static void mp_rundir_chown(const char *mpname, struct mpool_params *params)
{
	struct dirent  *d;

	char    path[PATH_MAX];
	DIR    *dir;
	int     rc;

	uid_t   uid  = params->mp_uid;
	gid_t   gid  = params->mp_gid;
	mode_t  mode = params->mp_mode;

	if (uid == -1 && gid == -1 && mode == -1)
		return;

	snprintf(path, sizeof(path), "%s/%s", MPOOL_RUNDIR_ROOT, mpname);

	dir = opendir(path);
	if (!dir) {
		mse_log(MPOOL_WARNING "%s: opendir(%s): %s", __func__, path, strerror(errno));
		return;
	}

	while ((d = readdir(dir))) {
		if (d->d_name[0] == '.')
			continue;

		rc = fchownat(dirfd(dir), d->d_name, uid, gid, 0);
		if (rc)
			mse_log(MPOOL_WARNING "%s: chown(%s/%s, %u, %u): %s",
				__func__, path, d->d_name, uid, gid, strerror(errno));
	}

	rc = fchownat(dirfd(dir), ".", uid, gid, 0);
	if (rc)
		mse_log(MPOOL_WARNING "%s: chown(%s, %u, %u): %s",
			__func__, path, uid, gid, strerror(errno));

	if (mode != -1) {
		mode &= 0777;
		mode |= ((mode & 0700) ? 0100 : 0);
		mode |= ((mode & 0070) ? 0010 : 0);
		mode |= ((mode & 0007) ? 0001 : 0);

		rc = fchmodat(dirfd(dir), ".", mode, 0);
		if (rc)
			mse_log(MPOOL_WARNING "%s: chmod(%s, %0o): %s",
				__func__, path, mode, strerror(errno));
	}

	closedir(dir);
}

mpool_err_t mpool_params_get(struct mpool *mp, struct mpool_params *params, struct mpool_devrpt *ei)
{
	struct mpioc_params get = { };
	merr_t              err;

	mpool_devrpt_init(ei);

	if (!mp || !params)
		return merr(EINVAL);

	err = mpool_ioctl(mp->mp_fd, MPIOC_PARAMS_GET, &get);
	if (err) {
		mpool_devrpt(ei, MPOOL_RC_PARM, -1, mp->mp_name);
		return err;
	}

	*params = get.mps_params;

	return 0;
}

mpool_err_t mpool_params_set(struct mpool *mp, struct mpool_params *params, struct mpool_devrpt *ei)
{
	struct mpioc_params set = { };
	merr_t              err;

	mpool_devrpt_init(ei);

	if (!mp || !params)
		return merr(EINVAL);

	err = mpool_strchk(params->mp_label, 0, MPOOL_LABELSZ_MAX - 1, ei);
	if (err)
		return err;

	set.mps_params = *params;

	err = mpool_ioctl(mp->mp_fd, MPIOC_PARAMS_SET, &set);
	if (err) {
		mpool_devrpt(ei, MPOOL_RC_PARM, -1, mp->mp_name);
		return err;
	}

	mp_rundir_chown(mp->mp_name, &set.mps_params);

	if (params->mp_uid != -1 || params->mp_gid != -1 || params->mp_mode != -1)
		err = mpool_ugm_check(NULL, mp->mp_fd, &set.mps_params);

	*params = set.mps_params;

	return err;
}

mpool_err_t mpool_usage_get(struct mpool *mp, struct mpool_usage *usage)
{
	struct mpioc_prop   prop = { };
	struct mpioc_list   ls = {
		.ls_listv = &prop,
		.ls_listc = 1,
		.ls_cmd = MPIOC_LIST_CMD_PROP_GET,
	};

	merr_t  err;

	if (!mp || !usage)
		return merr(EINVAL);

	err = mpool_ioctl(mp->mp_fd, MPIOC_PROP_GET, &ls);
	if (err)
		return err;

	*usage = prop.pr_usage;

	return 0;
}

mpool_err_t mpool_dev_props_get(struct mpool *mp, const char *devname, struct mpool_devprops *props)
{
	struct mpioc_devprops  dprops;

	char    rpath[PATH_MAX], *base;
	merr_t  err;
	size_t  n;

	if (!mp || !devname || !props)
		return merr(EINVAL);

	if (mp->mp_fd < 0)
		return merr(EBADF);

	memset(&dprops, 0, sizeof(dprops));

	if (!realpath(devname, rpath))
		return merr(errno);

	base = strrchr(rpath, '/');
	base = base ? base + 1 : rpath;

	n = strlcpy(dprops.dpr_pdname, base, sizeof(dprops.dpr_pdname));
	if (n >= sizeof(dprops.dpr_pdname))
		return merr(ENAMETOOLONG);

	err = mpool_ioctl(mp->mp_fd, MPIOC_DEVPROPS_GET, &dprops);
	if (!err)
		*props = dprops.dpr_devprops;

	return err;
}

mpool_err_t
mpool_activate(const char *mpname, struct mpool_params *params, u32 flags, struct mpool_devrpt *ei)
{
	struct mpioc_mpool  mp = { };
	struct imp_entry   *entry;

	char   **dpaths;
	int      fd, i;
	merr_t   err;
	int      entry_cnt;

	mpool_devrpt_init(ei);

	if (!mpname)
		return merr(EINVAL);

	mpool_params_init2(&mp.mp_params, params);

	err = mpool_strchk(mp.mp_params.mp_label, 0, MPOOL_LABELSZ_MAX - 1, ei);
	if (err)
		return err;

	fd = open(MPC_DEV_CTLPATH, O_RDWR | O_CLOEXEC);
	if (-1 == fd) {
		err = merr(errno);
		mpool_devrpt(ei, MPOOL_RC_OPEN, -1, MPC_DEV_CTLPATH);
		return err;
	}

	err = discover(mpname, &flags, &entry, &entry_cnt, &dpaths, '\n', __func__);
	if (err) {
		if (merr_errno(err) == ENOENT)
			mpool_devrpt(ei, MPCTL_RC_MP_NODEV, -1, mpname);
		close(fd);
		return err;
	}

	/* Turn off write throttling on the PDs */
	for (i = 0; i < entry_cnt; i++) {
		err = sysfs_pd_disable_wbt(entry[i].mp_path);
		if (err)
			goto errout;
	}

	/*
	 * If that fail for a device, this device is removed from the devices
	 * used to activate the mpool.
	 */
	mp.mp_pd_prop = imp_entries2pd_prop(entry_cnt, entry);
	if (!mp.mp_pd_prop) {
		mpool_devrpt(ei, MPOOL_RC_ENOMEM, -1, "imp_entries2pd_prop");
		err = merr(ENOMEM);
		goto errout;
	}

	mp.mp_dpathc = entry_cnt;
	mp.mp_dpaths = dpaths[0];
	mp.mp_dpathssz = strlen(dpaths[0]) + 1; /* trailing NUL */
	mp.mp_flags = flags;

	strlcpy(mp.mp_params.mp_name, entry->mp_name, sizeof(mp.mp_params.mp_name));

	err = mpool_ioctl(fd, MPIOC_MP_ACTIVATE, &mp);
	if (err)
		goto errout;

	err = mpool_ugm_check(entry->mp_name, -1, &mp.mp_params);

	if (params)
		*params = mp.mp_params;

	if (!err)
		mpool_rundir_create(entry->mp_name);

errout:
	if (mp.mp_pd_prop)
		free(mp.mp_pd_prop);
	free(entry);
	free(dpaths);
	close(fd);

	return err;
}

mpool_err_t mpool_deactivate(const char *mpname, u32 flags, struct mpool_devrpt *ei)
{
	struct mpioc_mpool  mp;
	struct imp_entry   *entry;
	int                 fd;
	merr_t              err;
	int                 entry_cnt;
	char              **dpaths;

	mpool_devrpt_init(ei);

	if (!mpname)
		return merr(EINVAL);

	err = discover(mpname, &flags, &entry, &entry_cnt, &dpaths, '\n', __func__);
	if (err) {
		if (merr_errno(err) == ENOENT)
			mpool_devrpt(ei, MPCTL_RC_MP_NODEV, -1, mpname);
		return err;
	}

	mpool_rundir_destroy(entry->mp_name);

	fd = open(MPC_DEV_CTLPATH, O_RDWR | O_CLOEXEC);
	if (-1 == fd) {
		err = merr(errno);
		mpool_devrpt(ei, MPOOL_RC_OPEN, -1, MPC_DEV_CTLPATH);
		goto errout;
	}

	memset(&mp, 0, sizeof(mp));
	strlcpy(mp.mp_params.mp_name, entry->mp_name, sizeof(mp.mp_params.mp_name));

	err = mpool_ioctl(fd, MPIOC_MP_DEACTIVATE, &mp);
	if (err && ei) {
		if (mpool_errno(err) == ENXIO)
			mpool_devrpt(ei, MPCTL_RC_NOTACTIVATED, -1, NULL);
	}

	close(fd);
errout:
	free(entry);
	free(dpaths);

	return err;
}

mpool_err_t
mpool_rename(const char *oldmp, const char *newmp, uint32_t flags, struct mpool_devrpt *ei)
{
	struct mpioc_mpool  mp;
	struct imp_entry   *entry = NULL;
	int                 fd;
	merr_t              err = 0;
	int                 entry_cnt;
	char              **dpaths = NULL;
	char                uuid[MPOOL_UUID_STRING_LEN + 1];
	bool                force = ((flags & (1 << MP_FLAGS_FORCE)) != 0);

	mpool_devrpt_init(ei);

	if (!oldmp || !newmp)
		return merr(EINVAL);

	err = mpool_strchk(newmp, 1, MPOOL_NAMESZ_MAX - 1, NULL);
	if (err) {
		mpool_devrpt(ei, MPCTL_RC_ENTNAM_INV, -1, newmp);
		return err;
	}

	if (!force && imp_mpool_exists(newmp, flags, NULL)) {
		mpool_devrpt(ei, MPCTL_RC_MPEXIST, -1, newmp);
		return merr(EEXIST);
	}

	if (!imp_mpool_exists(oldmp, flags, &entry)) {
		mpool_devrpt(ei, MPCTL_RC_MP_NODEV, -1, oldmp);
		return merr(ENOENT);
	}

	if (imp_mpool_activated(entry->mp_name))
		return merr(EBUSY);

	mpool_unparse_uuid(&entry->mp_uuid, uuid);
	free(entry);

	/* Find all devices associated with oldmp by UUID */
	err = discover(uuid, &flags, &entry, &entry_cnt, &dpaths, '\n', __func__);
	if (err) {
		if (merr_errno(err) == ENOENT)
			mpool_devrpt(ei, MPCTL_RC_MP_NODEV, -1, oldmp);
		return err;
	}

	fd = open(MPC_DEV_CTLPATH, O_RDWR | O_CLOEXEC);
	if (-1 == fd) {
		err = merr(errno);
		mpool_devrpt(ei, MPOOL_RC_OPEN, -1, MPC_DEV_CTLPATH);
		goto errout;
	}

	memset(&mp, 0, sizeof(mp));
	strlcpy(mp.mp_params.mp_name, newmp, sizeof(mp.mp_params.mp_name));

	mp.mp_pd_prop = imp_entries2pd_prop(entry_cnt, entry);
	if (!mp.mp_pd_prop) {
		mpool_devrpt(ei, MPOOL_RC_ENOMEM, -1, "imp_entries2pd_prop");
		err = merr(ENOMEM);
		close(fd);
		goto errout;
	}

	mp.mp_dpathc = entry_cnt;
	mp.mp_dpaths = dpaths[0];
	mp.mp_dpathssz = strlen(dpaths[0]) + 1; /* trailing NUL */
	mp.mp_flags = flags;

	err = mpool_ioctl(fd, MPIOC_MP_RENAME, &mp);

	close(fd);

errout:
	if (mp.mp_pd_prop)
		free(mp.mp_pd_prop);
	free(entry);
	free(dpaths);

	return err;
}

static merr_t mp_acquire(struct mpool *mp)
{
	merr_t err = 0;

	if (!mp)
		return merr(EINVAL);

	mutex_lock(&mp->mp_lock);

	/* mp_close invalidates magic and fd */
	if (mp->mp_magic != MPC_MPOOL_MAGIC)
		err = merr(EINVAL);
	else if (mp->mp_fd < 0)
		err = merr(EBADFD);

	if (err)
		mutex_unlock(&mp->mp_lock);

	return err;
}

static inline void mp_release(struct mpool *mp)
{
	mutex_unlock(&mp->mp_lock);
}

merr_t mpool_name_get(struct mpool *mp, char *mpname, size_t mplen)
{
	merr_t err;

	if (!mp || !mpname)
		return merr(EINVAL);

	err = mp_acquire(mp);
	if (err)
		return err;

	strlcpy(mpname, mp->mp_name, mplen);

	mp_release(mp);

	return 0;
}

mpool_err_t
mpool_open(const char *mp_name, uint32_t flags, struct mpool **mpp, struct mpool_devrpt *ei)
{
	struct mpool   *mp;

	char    path[PATH_MAX];
	merr_t  err;
	int     rc;

	if (!mp_name || !mpp)
		return merr(EINVAL);

	rc = snprintf(path, sizeof(path), "/dev/%s/%s", MPC_DEV_SUBDIR, mp_name);

	if (rc < 0 || rc >= sizeof(path))
		return merr(ENAMETOOLONG);

	mp = calloc(1, sizeof(*mp));
	if (!mp)
		return merr(ENOMEM);

	if (!flags)
		flags = O_RDWR;

	flags &= O_EXCL | O_RDWR | O_RDONLY | O_WRONLY;

	mp->mp_fd = open(path, flags | O_CLOEXEC);
	if (-1 == mp->mp_fd) {
		err = merr(errno);
		mpool_devrpt(ei, MPOOL_RC_OPEN, -1, path);
		free(mp);
		return err;
	}

	mp->mp_magic = MPC_MPOOL_MAGIC;
	mutex_init(&mp->mp_lock);
	mp->mp_flags = flags;
	strlcpy(mp->mp_name, mp_name, sizeof(mp->mp_name));

	*mpp = mp;

	return 0;
}

mpool_err_t mpool_close(struct mpool *mp)
{
	merr_t  err;
	int     i;

	if (!mp)
		return 0;

	err = mp_acquire(mp);
	if (err)
		return err;

	for (i = 0; i < MAX_OPEN_MLOGS; i++) {
		if (mp->mp_mlmap[i].mlm_hdl) {
			mp_release(mp);
			return merr(EBUSY);
		}
	}

	mp->mp_magic = MPC_NO_MAGIC;

	close(mp->mp_fd);
	mp->mp_fd = -1;

	mp_release(mp);
	free(mp);

	return 0;
}

/*
 * Mpctl Mlog interface implementation
 */

/**
 * mlog_hmap_find() - Lookup mlog map for the handle given an object ID
 *
 * @mp:      mpool handle
 * @objid:   object ID
 * @needref: if true, increment refcount on the mlog handle
 * @locked:  is the mp_lock already acquired?
 */
static struct mpool_mlog *mlog_hmap_find(struct mpool *mp, u64 objid, bool needref, bool locked)
{
	struct mpool_mlog  *mlh;
	struct mp_mloghmap *mlmap;
	int                 i;

	mlh = NULL;

	if (!locked && mp_acquire(mp))
		return NULL;

	for (i = 0; i < MAX_OPEN_MLOGS; i++) {
		mlmap = &mp->mp_mlmap[i];
		if (objid == mlmap->mlm_objid) {
			assert(mlmap->mlm_refcnt > 0);

			if (needref)
				++mlmap->mlm_refcnt;

			mlh = mp->mp_mlmap[i].mlm_hdl;
			assert(mlh != NULL);
			break;
		}
	}

	if (!locked)
		mp_release(mp);

	return mlh;
}

/**
 * mlog_hmap_put_locked() - drop a reference on given mlog handle
 * @mp:      mpool handle
 * @mlogh:   mlog handle
 * @lastref: set to true if the last reference was dropped (output)
 */
static void mlog_hmap_put_locked(struct mpool *mp, struct mpool_mlog *mlogh, bool *lastref)
{
	struct mp_mloghmap *mlmap;

	if (!mp || !mlogh)
		return;

	mlmap = &mp->mp_mlmap[mlogh->ml_idx];
	assert(mlmap->mlm_hdl == mlogh);
	assert(mlmap->mlm_refcnt > 0);

	if (--mlmap->mlm_refcnt > 0)
		return;

	mp->mp_mlidx = mlogh->ml_idx;
	--mp->mp_mltot;

	memset(mlmap, 0, sizeof(*mlmap));

	if (lastref)
		*lastref = true;
}

static void mlog_hmap_put(struct mpool *mp, struct mpool_mlog *mlogh, bool *lastref)
{
	if (!mp || !mlogh || mp_acquire(mp))
		return;

	mlog_hmap_put_locked(mp, mlogh, lastref);

	mp_release(mp);
}

/**
 * mlog_hmap_insert() - Insert <objid, mlogh> pair into mlog map
 * @mp:    mpool handle
 * @objid: object ID
 * @mlogh: mlog handle
 */
static merr_t mlog_hmap_insert(struct mpool *mp, u64 objid, struct mpool_mlog *mlogh)
{
	struct mp_mloghmap *mlmap;
	struct mpool_mlog  *dup;

	merr_t err = 0;
	int    i;
	u16    nidx;

	if (!mp || !mlogh)
		return merr(EINVAL);

	err = mp_acquire(mp);
	if (err)
		return err;

	if (mp->mp_mltot >= MAX_OPEN_MLOGS) {
		err = merr(ENOSPC);
		goto exit;
	}

	dup = mlog_hmap_find(mp, objid, true, true);
	if (dup) {
		mlog_hmap_put_locked(mp, dup, NULL);
		err = merr(EEXIST);
		goto exit;
	}

	/* Cache the map in the next free index */
	nidx  = mp->mp_mlidx;
	mlmap = &mp->mp_mlmap[nidx];
	assert(mlmap->mlm_hdl == NULL);

	mlmap->mlm_objid  = objid;
	mlmap->mlm_hdl    = mlogh;
	mlmap->mlm_refcnt = 1;

	mlogh->ml_idx = nidx;

	if (++mp->mp_mltot == MAX_OPEN_MLOGS)
		goto exit;

	/* Find next free index */
	for (i = nidx + 1; ; i++) {
		i %= MAX_OPEN_MLOGS;
		if (i == nidx)
			break;

		mlmap = &mp->mp_mlmap[i];
		if (!mlmap->mlm_hdl) {
			mp->mp_mlidx = i;
			goto exit;
		}
	}
exit:
	mp_release(mp);

	return err;
}

/**
 * mlog_acquire() - Validate mlog handle and acquire ml_lock
 * @mlogh: mlog handle
 * @rw:    read/append?
 */
static inline merr_t mlog_acquire(struct mpool_mlog *mlogh, bool rw)
{
	if (!mlogh || mlogh->ml_magic != MPC_MLOG_MAGIC)
		return merr(EINVAL);

	if (rw && (mlogh->ml_flags & MLOG_OF_SKIP_SER))
		return 0;

	mutex_lock(&mlogh->ml_lock);

	if (mlogh->ml_mpfd < 0) {
		mutex_unlock(&mlogh->ml_lock);
		return merr(EBADFD);
	}

	return 0;
}

/**
 * mlog_release() - Release ml_lock
 * @mlogh: mlog handle
 * @rw: read/append?
 */
static inline void mlog_release(struct mpool_mlog *mlogh, bool rw)
{
	if (!mlogh || (rw && (mlogh->ml_flags & MLOG_OF_SKIP_SER)))
		return;

	mutex_unlock(&mlogh->ml_lock);
}

static void mlog_handle_free(struct mpool_mlog *mlogh);

static merr_t
mlog_handle_alloc_impl(
	struct mpool            *mp,
	struct mlog_props_ex    *props,
	char                    *mpname,
	struct mpool_mlog      **mlogh)
{
	struct mpool_mlog          *mlh;
	struct mpool_descriptor    *mpdesc;
	struct mlog_descriptor     *mldesc;

	merr_t err;
	u64    objid;

	if (!mlogh || !mp || !props || !mpname)
		return merr(EINVAL);

	*mlogh = NULL;

	mlh = calloc(1, sizeof(*mlh));
	if (!mlh)
		return merr(ENOMEM);

	/* Allocate and init mpool descriptor for user space mlogs */
	mpdesc = mpool_user_desc_alloc(mpname);
	if (!mpdesc) {
		free(mlh);
		return merr(ENOMEM);
	}
	mlh->ml_mpdesc = mpdesc;

	/* Allocate and init mlog descriptor for user space mlogs */
	mldesc = mlog_user_desc_alloc(mpdesc, props, mlh);
	if (!mldesc) {
		mpool_user_desc_free(mpdesc);
		free(mlh);

		return merr(ENOMEM);
	}
	mlh->ml_mldesc = mldesc;

	objid = props->lpx_props.lpr_objid;

	/* Initialize other fields in the handle */
	mlh->ml_magic = MPC_MLOG_MAGIC;
	mlh->ml_objid = objid;
	mlh->ml_mpfd  = mp->mp_fd;
	mlh->ml_mp    = mp;

	mutex_init(&mlh->ml_lock);

	/* Insert this mlog handle in the mpool mlog map */
	err = mlog_hmap_insert(mp, objid, mlh);
	if (err) {
		mlog_handle_free(mlh);
		return err;
	}

	*mlogh = mlh;

	return 0;
}

static merr_t mlog_handle_alloc(struct mpool *mp, u64 objid, struct mpool_mlog **mlogh)
{
	struct mpioc_mlog       ml = { .ml_objid = objid };
	struct mlog_props_ex   *px;
	struct mpool_mlog      *mlh;
	merr_t                  err;

	if (!mp || !mlogh)
		return merr(EINVAL);

	*mlogh = NULL;

	err = mpool_ioctl(mp->mp_fd, MPIOC_MLOG_FIND, &ml);
	if (err)
		return err;
	px = &ml.ml_props;

again:
	mlh = mlog_hmap_find(mp, objid, true, false);
	if (!mlh) {
		err = mlog_handle_alloc_impl(mp, px, mp->mp_name, &mlh);
		if (err) {
			if (merr_errno(err) == EEXIST)
				goto again;

			return err;
		}
	}

	*mlogh = mlh;

	return 0;
}

/**
 * mlog_handle_free() - Free an mlog handle
 * @mlogh: mlog handle
 */
static void mlog_handle_free(struct mpool_mlog *mlogh)
{
	mlog_user_desc_free(mlogh->ml_mldesc);

	mpool_user_desc_free(mlogh->ml_mpdesc);

	free(mlogh);
}

static merr_t mlog_handle_put(struct mpool_mlog *mlogh)
{
	bool    rw = false;
	bool    lastref = false;
	merr_t  err;

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	mlog_hmap_put(mlogh->ml_mp, mlogh, &lastref);

	mlog_release(mlogh, rw);

	if (lastref) {
		err = mlog_close(mlogh->ml_mpdesc, mlogh->ml_mldesc);
		mlog_handle_free(mlogh);
	}

	return err;
}

/**
 * mpool_is_writable() - Validate whether an mpool is opened in write mode
 *
 * @mp: mpool handle
 */
static inline bool mpool_is_writable(struct mpool *mp)
{
	return (mp->mp_flags & (O_RDWR | O_WRONLY));
}

mpool_err_t
mpool_mlog_alloc(
	struct mpool           *mp,
	enum mp_media_classp    mclassp,
	struct mlog_capacity   *capreq,
	uint64_t               *mlogid,
	struct mlog_props      *props)
{
	struct mpioc_mlog  ml = { .ml_mclassp = mclassp };
	merr_t             err;

	if (!mp || !capreq || !mlogid)
		return merr(EINVAL);

	if (!mpool_is_writable(mp))
		return merr(EPERM);

	ml.ml_cap = *capreq;

	err = mpool_ioctl(mp->mp_fd, MPIOC_MLOG_ALLOC, &ml);
	if (err)
		return err;

	*mlogid = ml.ml_props.lpx_props.lpr_objid;

	if (props)
		*props = ml.ml_props.lpx_props;

	return 0;
}

mpool_err_t mpool_mlog_commit(struct mpool *mp, uint64_t mlogid)
{
	struct mpioc_mlog_id   mi = { .mi_objid = mlogid };

	if (!mp)
		return merr(EINVAL);

	if (!mpool_is_writable(mp))
		return merr(EPERM);

	return mpool_ioctl(mp->mp_fd, MPIOC_MLOG_COMMIT, &mi);
}

mpool_err_t mpool_mlog_abort(struct mpool *mp, uint64_t mlogid)
{
	struct mpioc_mlog_id    mi = { .mi_objid = mlogid };
	struct mpool_mlog      *mlh;

	if (!mp)
		return merr(EINVAL);

	if (!mpool_is_writable(mp))
		return merr(EPERM);

	mlh = mlog_hmap_find(mp, mlogid, false, false);
	if (mlh)
		return merr(EBUSY);

	return mpool_ioctl(mp->mp_fd, MPIOC_MLOG_ABORT, &mi);
}

mpool_err_t mpool_mlog_delete(struct mpool *mp, uint64_t mlogid)
{
	struct mpioc_mlog_id    mi = { .mi_objid = mlogid };
	struct mpool_mlog      *mlh;

	if (!mp)
		return merr(EINVAL);

	if (!mpool_is_writable(mp))
		return merr(EPERM);

	mlh = mlog_hmap_find(mp, mlogid, false, false);
	if (mlh)
		return merr(EBUSY);

	return mpool_ioctl(mp->mp_fd, MPIOC_MLOG_DELETE, &mi);
}

mpool_err_t
mpool_mlog_open(
	struct mpool        *mp,
	u64                  mlogid,
	uint8_t              flags,
	uint64_t            *gen,
	struct mpool_mlog  **mlogh)
{
	struct mpool_mlog  *mlh;
	merr_t              err;

	if (!mp || !mlogh || !gen)
		return merr(EINVAL);

	err = mlog_handle_alloc(mp, mlogid, &mlh);
	if (err)
		return err;

	flags &= MLOG_OF_SKIP_SER | MLOG_OF_COMPACT_SEM;
	mlh->ml_flags = flags;

	err = mlog_open(mlh->ml_mpdesc, mlh->ml_mldesc, flags, gen);
	if (err) {
		mlog_handle_put(mlh);
		return err;
	}

	*mlogh = mlh;

	return 0;
}

mpool_err_t mpool_mlog_close(struct mpool_mlog *mlogh)
{
	if (!mlogh)
		return merr(EINVAL);

	return mlog_handle_put(mlogh);
}

mpool_err_t mpool_mlog_append(struct mpool_mlog *mlogh, struct iovec *iov, size_t len, int sync)
{
	merr_t err;
	bool   rw = true;

	if (!mlogh || !iov)
		return merr(EINVAL);

	if (!mpool_is_writable(mlogh->ml_mp))
		return merr(EPERM);

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	err = mlog_append_datav(mlogh->ml_mpdesc, mlogh->ml_mldesc, iov, len, sync);

	mlog_release(mlogh, rw);

	return err;
}

mpool_err_t mpool_mlog_rewind(struct mpool_mlog *mlogh)
{
	merr_t err;
	bool   rw = false;

	if (!mlogh)
		return merr(EINVAL);

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	err = mlog_read_data_init(mlogh->ml_mpdesc, mlogh->ml_mldesc);

	mlog_release(mlogh, rw);

	return err;
}

mpool_err_t mpool_mlog_read(struct mpool_mlog *mlogh, void *data, size_t len, size_t *rdlen)
{
	merr_t err;
	bool   rw = true;

	if (!mlogh)
		return merr(EINVAL);

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	err = mlog_read_data_next(mlogh->ml_mpdesc, mlogh->ml_mldesc, data, len, rdlen);

	mlog_release(mlogh, rw);

	return err;
}

mpool_err_t
mpool_mlog_seek_read(struct mpool_mlog *mlogh, size_t seek, void *data, size_t len, size_t *rdlen)
{
	merr_t err;
	bool   rw = true;

	if (!mlogh)
		return merr(EINVAL);

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	err = mlog_seek_read_data_next(mlogh->ml_mpdesc, mlogh->ml_mldesc, seek, data, len, rdlen);

	mlog_release(mlogh, rw);

	return err;
}

mpool_err_t mpool_mlog_sync(struct mpool_mlog *mlogh)
{
	merr_t err;
	bool   rw = false;

	if (!mlogh)
		return merr(EINVAL);

	if (!mpool_is_writable(mlogh->ml_mp))
		return merr(EPERM);

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	err = mlog_flush(mlogh->ml_mpdesc, mlogh->ml_mldesc);

	mlog_release(mlogh, rw);

	return err;
}

mpool_err_t mpool_mlog_len(struct mpool_mlog *mlogh, size_t *len)
{
	merr_t err;
	bool   rw = false;

	if (!mlogh || !len)
		return merr(EINVAL);

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	err = mlog_len(mlogh->ml_mpdesc, mlogh->ml_mldesc, len);

	mlog_release(mlogh, rw);

	return err;
}

mpool_err_t mpool_mlog_props_get(struct mpool_mlog *mlogh, struct mlog_props *props)
{
	struct mlog_props_ex   props_ex;

	merr_t err;

	if (!mlogh || !props)
		return merr(EINVAL);

	err = mpool_mlog_xprops_get(mlogh, &props_ex);
	if (err)
		return err;

	*props = props_ex.lpx_props;

	return 0;
}

mpool_err_t mpool_mlog_erase(struct mpool_mlog *mlogh, uint64_t mingen)
{
	struct mpool           *mp;
	struct mpioc_mlog_id    mi = { .mi_gen = mingen };

	merr_t  err;
	bool    rw = false;

	if (!mlogh)
		return merr(EINVAL);

	mp = mlogh->ml_mp;

	if (!mpool_is_writable(mp))
		return merr(EPERM);

	mi.mi_objid = mlogh->ml_objid;

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	err = mpool_ioctl(mp->mp_fd, MPIOC_MLOG_ERASE, &mi);
	if (err)
		goto exit;

	err = mlog_stat_reinit(mlogh->ml_mpdesc, mlogh->ml_mldesc);
	if (err)
		goto exit;

	err = mlog_user_desc_set(mlogh->ml_mpdesc, mlogh->ml_mldesc, mi.mi_gen, mi.mi_state);

exit:
	mlog_release(mlogh, rw);

	return err;
}

/*
 * Internal interfaces not exported to apps
 */

merr_t mpool_mlog_empty(struct mpool_mlog *mlogh, bool *empty)
{
	merr_t err;
	bool   rw = false;

	if (!mlogh || !empty)
		return merr(EINVAL);

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	err = mlog_empty(mlogh->ml_mpdesc, mlogh->ml_mldesc, empty);

	mlog_release(mlogh, rw);

	return err;
}
merr_t mpool_mlog_xprops_get(struct mpool_mlog *mlogh, struct mlog_props_ex *props_ex)
{
	struct mpool       *mp;
	struct mpioc_mlog   ml;

	merr_t  err;
	bool    rw = false;

	if (!mlogh || !props_ex)
		return merr(EINVAL);

	memset(&ml, 0, sizeof(ml));
	ml.ml_objid = mlogh->ml_objid;

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	mp = mlogh->ml_mp;
	err = mpool_ioctl(mp->mp_fd, MPIOC_MLOG_PROPS, &ml);
	if (!err)
		*props_ex = ml.ml_props;

	mlog_release(mlogh, rw);

	return err;
}

merr_t mpool_mlog_append_cstart(struct mpool_mlog *mlogh)
{
	merr_t err;
	bool   rw = false;

	if (!mlogh)
		return merr(EINVAL);

	if (!mpool_is_writable(mlogh->ml_mp))
		return merr(EPERM);

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	err = mlog_append_cstart(mlogh->ml_mpdesc, mlogh->ml_mldesc);

	mlog_release(mlogh, rw);

	return err;
}

merr_t mpool_mlog_append_cend(struct mpool_mlog *mlogh)
{
	merr_t err;
	bool   rw = false;

	if (!mlogh)
		return merr(EINVAL);

	if (!mpool_is_writable(mlogh->ml_mp))
		return merr(EPERM);

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	err = mlog_append_cend(mlogh->ml_mpdesc, mlogh->ml_mldesc);

	mlog_release(mlogh, rw);

	return err;
}

#ifndef NVALGRIND
/* Valgrind wrapper function mpool_mlog_rw().
 *
 * Valgrind doesn't understand that the ioctl invoked by mpool_mlog_rw() puts
 * data in the read buffer.  This wrapper function initializes the buffer
 * before calling mpool_mlog_rw(), thus preventing false positives when the
 * caller accesses data in the read buffer.
 */
merr_t
I_WRAP_SONAME_FNNAME_ZU(NONE, mpool_mlog_rw)(
	struct mpool_mlog  *mlogh,
	struct iovec       *iov,
	int                 iovc,
	size_t              off,
	u8                  rw)
{
	static atomic_t once;
	OrigFn          fn;
	int             i, result;

	VALGRIND_GET_ORIG_FN(fn);

	if (atomic_cmpxchg(&once, 0, 1) == 0)
		mse_log(MPOOL_NOTICE "valgrind wrapper enabled: mpool_mlog_rw()");

	if (iov && rw == MPOOL_OP_READ) {
		for (i = 0; i < iovc; i++)
			memset(iov[i].iov_base, 0xf0, iov[i].iov_len);
	}
	CALL_FN_W_5W(result, fn, mlogh, iov, iovc, off, rw);

	return result;
}
#endif

merr_t
mpool_mlog_rw(
	struct mpool_mlog  *mlogh,
	struct iovec       *iov,
	int                 iovc,
	size_t              off,
	u8                  rw)
{
	struct mpioc_mlog_io mi = { };

	if (!mlogh || !iov || iovc < 1)
		return merr(EINVAL);

	mi.mi_objid = mlogh->ml_objid;
	mi.mi_iov   = iov;
	mi.mi_iovc  = iovc;
	mi.mi_off   = off;
	mi.mi_op    = rw;

	if (rw == MPOOL_OP_READ)
		return mpool_ioctl(mlogh->ml_mpfd, MPIOC_MLOG_READ, &mi);

	if (rw == MPOOL_OP_WRITE)
		return mpool_ioctl(mlogh->ml_mpfd, MPIOC_MLOG_WRITE, &mi);

	return merr(EINVAL);
}

merr_t mpool_mlog_gen(struct mpool_mlog *mlogh, u64 *gen)
{
	merr_t err;
	bool   rw = false;

	if (!mlogh || !gen)
		return merr(EINVAL);

	err = mlog_acquire(mlogh, rw);
	if (err)
		return err;

	err = mlog_gen(mlogh->ml_mpdesc, mlogh->ml_mldesc, gen);

	mlog_release(mlogh, rw);

	return err;
}

merr_t mpool_mlog_erase_byoid(struct mpool *mp, u64 mlogid, uint64_t mingen)
{
	struct mpioc_mlog_id   mi = { .mi_gen = mingen };

	if (!mp)
		return merr(EINVAL);

	if (!mpool_is_writable(mp))
		return merr(EPERM);

	mi.mi_objid = mlogid;

	return mpool_ioctl(mp->mp_fd, MPIOC_MLOG_ERASE, &mi);
}

/* Mpctl Mblock Interfaces */

mpool_err_t
mpool_mblock_alloc(
	struct mpool           *mp,
	enum mp_media_classp    mclassp,
	bool                    spare,
	uint64_t               *mbid,
	struct mblock_props    *props)
{
	struct mpioc_mblock mb = { .mb_mclassp = mclassp };
	merr_t              err;

	if (!mp || !mbid)
		return merr(EINVAL);

	mb.mb_spare = spare;

	err = mpool_ioctl(mp->mp_fd, MPIOC_MB_ALLOC, &mb);
	if (err)
		return err;

	*mbid = mb.mb_objid;

	if (props)
		*props = mb.mb_props.mbx_props;

	return 0;
}

mpool_err_t mpool_mblock_find(struct mpool *mp, uint64_t objid, struct mblock_props *props)
{
	struct mpioc_mblock mb = { .mb_objid = objid };
	merr_t              err;

	if (!mp)
		return merr(EINVAL);

	err = mpool_ioctl(mp->mp_fd, MPIOC_MB_FIND, &mb);
	if (err)
		return err;

	if (props)
		*props = mb.mb_props.mbx_props;

	return 0;
}

mpool_err_t mpool_mblock_commit(struct mpool *mp, uint64_t mbid)
{
	struct mpioc_mblock_id  mi = { .mi_objid = mbid };

	if (!mp)
		return merr(EINVAL);

	return mpool_ioctl(mp->mp_fd, MPIOC_MB_COMMIT, &mi);
}

mpool_err_t mpool_mblock_abort(struct mpool *mp, uint64_t mbid)
{
	struct mpioc_mblock_id  mi = { .mi_objid = mbid };

	if (!mp)
		return merr(EINVAL);

	return mpool_ioctl(mp->mp_fd, MPIOC_MB_ABORT, &mi);
}

mpool_err_t mpool_mblock_delete(struct mpool *mp, uint64_t mbid)
{
	struct mpioc_mblock_id  mi = { .mi_objid = mbid };

	if (!mp)
		return merr(EINVAL);

	return mpool_ioctl(mp->mp_fd, MPIOC_MB_DELETE, &mi);
}

mpool_err_t mpool_mblock_props_get(struct mpool *mp, uint64_t mbid, struct mblock_props *props)
{
	if (!mp || !props)
		return merr(EINVAL);

	return mpool_mblock_find(mp, mbid, props);
}

mpool_err_t mpool_mblock_write(struct mpool *mp, uint64_t mbid, const struct iovec *iov, int iovc)
{
	struct mpioc_mblock_rw mbrw = {
		.mb_objid   = mbid,
		.mb_iov_cnt = iovc,
		.mb_iov     = iov,
	};

	if (!mp || !iov)
		return merr(EINVAL);

	return mpool_ioctl(mp->mp_fd, MPIOC_MB_WRITE, &mbrw);
}

#ifndef NVALGRIND
/* Valgrind wrapper function mpool_mblock_read().
 *
 * Valgrind doesn't understand that the ioctl invoked by mpool_mblock_read()
 * puts data in the read buffer.  This wrapper function initializes the buffer
 * before calling mpool_mblock_read(), thus preventing false positives when the
 * caller accesses data in the read buffer.
 */
mpool_err_t
I_WRAP_SONAME_FNNAME_ZU(NONE, mpool_mblock_read)(
	struct mpool       *mp,
	uint64_t            mbid,
	const struct iovec *iov,
	int                 iovc,
	off_t               offset)
{
	static atomic_t once;
	OrigFn          fn;
	int             i, result;

	VALGRIND_GET_ORIG_FN(fn);

	if (atomic_cmpxchg(&once, 0, 1) == 0)
		mse_log(MPOOL_NOTICE "valgrind wrapper enabled: mpool_mblock_read()");

	if (iov) {
		for (i = 0; i < iovc; i++)
			memset(iov[i].iov_base, 0xaa, iov[i].iov_len);
	}
	CALL_FN_W_5W(result, fn, mp, mbid, iov, iovc, offset);

	return result;
}
#endif

mpool_err_t
mpool_mblock_read(struct mpool *mp, uint64_t mbid, const struct iovec *iov, int iovc, off_t offset)
{
	struct mpioc_mblock_rw mbrw = {
		.mb_objid   = mbid,
		.mb_offset  = offset,
		.mb_iov_cnt = iovc,
		.mb_iov     = iov,
	};

	if (!mp || !iov)
		return merr(EINVAL);

	return mpool_ioctl(mp->mp_fd, MPIOC_MB_READ, &mbrw);
}

mpool_err_t
mpool_mcache_mmap(
	struct mpool                *mp,
	size_t                       mbidc,
	uint64_t                    *mbidv,
	enum mpc_vma_advice          advice,
	struct mpool_mcache_map    **mapp)
{
	struct mpool_mcache_map    *map;
	struct mpioc_vma            vma;

	int     flags, prot, fd;
	merr_t  err;

	fd = mp->mp_fd;
	*mapp = NULL;

	map = calloc(1, sizeof(*map));
	if (!map)
		return merr(ENOMEM);

	memset(&vma, 0, sizeof(vma));
	vma.im_advice = advice;
	vma.im_mbidc = mbidc;
	vma.im_mbidv = mbidv;

	err = mpool_ioctl(fd, MPIOC_VMA_CREATE, &vma);
	if (err) {
		free(map);
		return err;
	}

	flags = MAP_SHARED | MAP_NORESERVE;
	prot = PROT_READ;

	map->mh_bktsz = vma.im_bktsz;
	map->mh_mbidc = vma.im_mbidc;
	map->mh_offset = vma.im_offset;
	map->mh_len = vma.im_len;
	map->mh_fd = fd;

	map->mh_addr = mmap(NULL, map->mh_len, prot, flags, fd, map->mh_offset);

	if (map->mh_addr == MAP_FAILED) {
		err = merr(errno);
		mpool_ioctl(fd, MPIOC_VMA_DESTROY, &vma);
		free(map);
		return err;
	}

	*mapp = map;

	return 0;
}

mpool_err_t mpool_mcache_munmap(struct mpool_mcache_map *map)
{
	int     rc;

	if (!map)
		return 0;

	rc = munmap(map->mh_addr, map->mh_len);
	if (rc)
		return merr(errno);

	free(map);

	return 0;
}

mpool_err_t
mpool_mcache_madvise(
	struct mpool_mcache_map    *map,
	uint                        mbidx,
	off_t                       offset,
	size_t                      length,
	int                         advice)
{
	int   rc;
	off_t ofs;

	if (!map || mbidx >= map->mh_mbidc || offset < 0)
		return merr(EINVAL);

	if (map->mh_addr == MAP_FAILED)
		return merr(EINVAL);

	ofs = (mbidx * map->mh_bktsz) + offset;

	if (length == SIZE_MAX) {
		length = map->mh_bktsz * map->mh_mbidc - ofs;
	} else {
		if (offset + length > map->mh_bktsz)
			return merr(EINVAL);
	}

	rc = madvise(map->mh_addr + ofs, length, advice);

	return rc ? merr(errno) : 0;
}

mpool_err_t mpool_mcache_purge(struct mpool_mcache_map *map, const struct mpool *mp)
{
	struct mpioc_vma    vma;

	if (!map || !mp)
		return merr(EINVAL);

	memset(&vma, 0, sizeof(vma));
	vma.im_offset = map->mh_offset;

	return mpool_ioctl(mp->mp_fd, MPIOC_VMA_PURGE, &vma);
}

static merr_t
mpool_mcache_vrss_get(
	struct mpool_mcache_map    *map,
	const struct mpool         *mp,
	size_t                     *rssp,
	size_t                     *vssp)
{
	struct mpioc_vma    vma;
	merr_t              err;

	memset(&vma, 0, sizeof(vma));
	vma.im_offset = map->mh_offset;

	err = mpool_ioctl(mp->mp_fd, MPIOC_VMA_VRSS, &vma);
	if (!err) {
		*vssp = vma.im_vssp;
		*rssp = vma.im_rssp;
	}

	return err;
}

mpool_err_t
mpool_mcache_mincore(
	struct mpool_mcache_map    *map,
	const struct mpool         *mp,
	size_t                     *rssp,
	size_t                     *vssp)
{
	unsigned char  *vec;
	size_t          segsz;
	size_t          vecsz;
	merr_t          err;
	int             rc;

	/* We *could* handle unmapped mcache maps; write some code? */
	if (map->mh_addr == MAP_FAILED)
		merr(EINVAL);

	if (!mpool_mcache_vrss_get(map, mp, rssp, vssp))
		return 0;

	segsz = map->mh_bktsz * map->mh_mbidc;
	vecsz = (segsz + PAGE_SIZE - 1) / PAGE_SIZE;

	vec = malloc(vecsz);
	if (!vec)
		return merr(ENOMEM);

	rc = mincore(map->mh_addr, segsz, vec);
	if (rc) {
		err = merr(errno);
		free(vec);
		return err;
	}

	if (rssp) {
		const ulong mask = 0x0101010101010101ul;

		ulong  *p = (ulong *)vec;
		size_t  rss = 0;
		int     i;

		for (i = 0; i < vecsz / sizeof(*p); ++i)
			rss += __builtin_popcountl(p[i] & mask);

		*rssp = rss;
	}

	/* The virtual set size reflects counts of pages in the holes (if any)
	 * between buckets.
	 */
	if (vssp)
		*vssp = segsz;

	free(vec);

	return 0;
}

void *mpool_mcache_getbase(struct mpool_mcache_map *map, const uint mbidx)
{
	if (!map || map->mh_addr == MAP_FAILED || mbidx >= map->mh_mbidc)
		return NULL;

	/* The mcache map exists, and we are in user space so it is mmapped...
	 * Return the base address.
	 */
	return map->mh_addr + (mbidx * map->mh_bktsz);
}

mpool_err_t
mpool_mcache_getpages(
	struct mpool_mcache_map    *map,
	const uint                  pagec,
	const uint                  mbidx,
	const off_t                 pagenumv[],
	void                       *addrv[])
{
	char *addr;
	int i;

	if (!map || map->mh_addr == MAP_FAILED || mbidx >= map->mh_mbidc)
		return merr(EINVAL);

	/* The mcache map exists, and we are in user space so it is mmapped...
	 * Calculate the page addresses within the map.
	 */
	addr = (char *)map->mh_addr + (mbidx * map->mh_bktsz);

	for (i = 0; i < pagec; i++)
		addrv[i] = addr + pagenumv[i] * PAGE_SIZE;

	return 0;
}

struct pd_prop *mp_get_dev_prop(int dcnt, char **devices)
{
	struct pd_prop *pdp;
	int    i;
	merr_t err;

	pdp = malloc(sizeof(struct pd_prop) * dcnt);
	if (!pdp)
		return NULL;

	for (i = 0; i < dcnt; i++) {
		err = imp_dev_get_prop(devices[i], pdp + i);
		if (err) {
			free(pdp);
			return NULL;
		}
	}

	return pdp;
}

mpool_err_t mp_trim_device(int devicec, char **devicev, struct mpool_devrpt *devrpt)
{
	merr_t  err = 0;
	int     i;

	mpool_devrpt_init(devrpt);

	if (!devicev || !devrpt || devicec < 1 || devicec > MPOOL_DRIVES_MAX)
		return merr(EINVAL);

	for (i = 0; i < devicec; i++) {
		enum mpool_rc   rcode;
		merr_t          err1;

		err1 = generic_trim_device(devicev[i], &rcode);
		if (err1) {
			mpool_devrpt(devrpt, rcode, i, NULL);
			err = err1;
		}
	}

	return err;
}

mpool_err_t mp_sb_magic_check(char *device, struct mpool_devrpt *devrpt)
{
	struct pd_prop    pd_prop;
	merr_t            err;

	if (!device || !devrpt)
		return merr(EINVAL);

	/* Get pd properties */
	err = imp_dev_get_prop(device, &pd_prop);
	if (err)
		return err;

	return mpool_sb_magic_check(device, &pd_prop, devrpt);
}

mpool_err_t mp_dev_activated(char *devpath, bool *activated, char *mp_name, size_t mp_name_sz)
{
	struct imp_entry *entry = NULL;
	int    entry_cnt = 0;
	merr_t err;
	u32    flags = 0;

	*activated = false;
	if (mp_name)
		mp_name[0] = 0;

	err = imp_entries_get(NULL, NULL, devpath, &flags, &entry, &entry_cnt);

	if (err || entry_cnt == 0)
		return err;

	if (entry_cnt != 1) {
		free(entry);
		return merr(EMLINK);
	}

	*activated = imp_mpool_activated(entry->mp_name);

	if (mp_name)
		strlcpy(mp_name, entry->mp_name, mp_name_sz);

	free(entry);

	return 0;
}
