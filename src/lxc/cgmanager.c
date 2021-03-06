/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <daniel.lezcano at free.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <netinet/in.h>
#include <net/if.h>

#include "error.h"
#include "commands.h"
#include "list.h"
#include "conf.h"
#include "utils.h"
#include "bdev.h"
#include "log.h"
#include "cgroup.h"
#include "start.h"
#include "state.h"

#define CGM_SUPPORTS_GET_ABS 3
#define CGM_SUPPORTS_NAMED 4

#ifdef HAVE_CGMANAGER
lxc_log_define(lxc_cgmanager, lxc);

#include <nih-dbus/dbus_connection.h>
#include <cgmanager/cgmanager-client.h>
#include <nih/alloc.h>
#include <nih/error.h>
#include <nih/string.h>

struct cgm_data {
	char *name;
	char *cgroup_path;
	const char *cgroup_pattern;
};

static pthread_mutex_t cgm_mutex = PTHREAD_MUTEX_INITIALIZER;

static void lock_mutex(pthread_mutex_t *l)
{
	int ret;

	if ((ret = pthread_mutex_lock(l)) != 0) {
		fprintf(stderr, "pthread_mutex_lock returned:%d %s\n", ret, strerror(ret));
		exit(1);
	}
}

static void unlock_mutex(pthread_mutex_t *l)
{
	int ret;

	if ((ret = pthread_mutex_unlock(l)) != 0) {
		fprintf(stderr, "pthread_mutex_unlock returned:%d %s\n", ret, strerror(ret));
		exit(1);
	}
}

void cgm_lock(void)
{
	lock_mutex(&cgm_mutex);
}

void cgm_unlock(void)
{
	unlock_mutex(&cgm_mutex);
}

#ifdef HAVE_PTHREAD_ATFORK
__attribute__((constructor))
static void process_lock_setup_atfork(void)
{
	pthread_atfork(cgm_lock, cgm_unlock, cgm_unlock);
}
#endif

static NihDBusProxy *cgroup_manager = NULL;
static int32_t api_version;

static struct cgroup_ops cgmanager_ops;
static int nr_subsystems;
static char **subsystems;
static bool dbus_threads_initialized = false;
static void cull_user_controllers(void);

static void cgm_dbus_disconnect(void)
{
       if (cgroup_manager) {
	       dbus_connection_flush(cgroup_manager->connection);
	       dbus_connection_close(cgroup_manager->connection);
               nih_free(cgroup_manager);
       }
       cgroup_manager = NULL;
       cgm_unlock();
}

#define CGMANAGER_DBUS_SOCK "unix:path=/sys/fs/cgroup/cgmanager/sock"
static bool cgm_dbus_connect(void)
{
	DBusError dbus_error;
	static DBusConnection *connection;

	cgm_lock();
	if (!dbus_threads_initialized) {
		// tell dbus to do struct locking for thread safety
		dbus_threads_init_default();
		dbus_threads_initialized = true;
	}

	dbus_error_init(&dbus_error);

	connection = dbus_connection_open_private(CGMANAGER_DBUS_SOCK, &dbus_error);
	if (!connection) {
		DEBUG("Failed opening dbus connection: %s: %s",
				dbus_error.name, dbus_error.message);
		dbus_error_free(&dbus_error);
		cgm_unlock();
		return false;
	}
	dbus_connection_set_exit_on_disconnect(connection, FALSE);
	dbus_error_free(&dbus_error);
	cgroup_manager = nih_dbus_proxy_new(NULL, connection,
				NULL /* p2p */,
				"/org/linuxcontainers/cgmanager", NULL, NULL);
	dbus_connection_unref(connection);
	if (!cgroup_manager) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("Error opening cgmanager proxy: %s", nerr->message);
		nih_free(nerr);
		cgm_dbus_disconnect();
		return false;
	}

	// get the api version
	if (cgmanager_get_api_version_sync(NULL, cgroup_manager, &api_version) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("Error cgroup manager api version: %s", nerr->message);
		nih_free(nerr);
		cgm_dbus_disconnect();
		return false;
	}
	if (api_version < CGM_SUPPORTS_NAMED)
		cull_user_controllers();
	return true;
}

static int send_creds(int sock, int rpid, int ruid, int rgid)
{
	struct msghdr msg = { 0 };
	struct iovec iov;
	struct cmsghdr *cmsg;
	struct ucred cred = {
		.pid = rpid,
		.uid = ruid,
		.gid = rgid,
	};
	char cmsgbuf[CMSG_SPACE(sizeof(cred))];
	char buf[1];
	buf[0] = 'p';

	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_CREDENTIALS;
	memcpy(CMSG_DATA(cmsg), &cred, sizeof(cred));

	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (sendmsg(sock, &msg, 0) < 0)
		return -1;
	return 0;
}

static bool lxc_cgmanager_create(const char *controller, const char *cgroup_path, int32_t *existed)
{
	bool ret = true;
	if ( cgmanager_create_sync(NULL, cgroup_manager, controller,
				       cgroup_path, existed) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_create_sync failed: %s", nerr->message);
		nih_free(nerr);
		ERROR("Failed to create %s:%s", controller, cgroup_path);
		ret = false;
	}

	return ret;
}

/*
 * Escape to the root cgroup if we are root, so that the container will
 * be in "/lxc/c1" rather than "/user/..../c1"
 * called internally with connection already open
 */
static bool lxc_cgmanager_escape(void)
{
	bool ret = true;
	pid_t me = getpid();
	int i;

	for (i = 0; i < nr_subsystems; i++) {
		if (cgmanager_move_pid_abs_sync(NULL, cgroup_manager,
					subsystems[i], "/", me) != 0) {
			NihError *nerr;
			nerr = nih_error_get();
			ERROR("call to cgmanager_move_pid_abs_sync(%s) failed: %s",
					subsystems[i], nerr->message);
			nih_free(nerr);
			ret = false;
			break;
		}
	}

	return ret;
}

struct chown_data {
	const char *controller;
	const char *cgroup_path;
	uid_t origuid;
};

static int do_chown_cgroup(const char *controller, const char *cgroup_path,
		uid_t origuid)
{
	int sv[2] = {-1, -1}, optval = 1, ret = -1;
	char buf[1];

	uid_t caller_nsuid = get_ns_uid(origuid);

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
		SYSERROR("Error creating socketpair");
		goto out;
	}
	if (setsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		SYSERROR("setsockopt failed");
		goto out;
	}
	if (setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		SYSERROR("setsockopt failed");
		goto out;
	}
	if ( cgmanager_chown_scm_sync(NULL, cgroup_manager, controller,
				       cgroup_path, sv[1]) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_chown_scm_sync failed: %s", nerr->message);
		nih_free(nerr);
		goto out;
	}
	/* now send credentials */

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(sv[0], &rfds);
	if (select(sv[0]+1, &rfds, NULL, NULL, NULL) < 0) {
		ERROR("Error getting go-ahead from server: %s", strerror(errno));
		goto out;
	}
	if (read(sv[0], &buf, 1) != 1) {
		ERROR("Error getting reply from server over socketpair");
		goto out;
	}
	if (send_creds(sv[0], getpid(), getuid(), getgid())) {
		SYSERROR("%s: Error sending pid over SCM_CREDENTIAL", __func__);
		goto out;
	}
	FD_ZERO(&rfds);
	FD_SET(sv[0], &rfds);
	if (select(sv[0]+1, &rfds, NULL, NULL, NULL) < 0) {
		ERROR("Error getting go-ahead from server: %s", strerror(errno));
		goto out;
	}
	if (read(sv[0], &buf, 1) != 1) {
		ERROR("Error getting reply from server over socketpair");
		goto out;
	}
	if (send_creds(sv[0], getpid(), caller_nsuid, 0)) {
		SYSERROR("%s: Error sending pid over SCM_CREDENTIAL", __func__);
		goto out;
	}
	FD_ZERO(&rfds);
	FD_SET(sv[0], &rfds);
	if (select(sv[0]+1, &rfds, NULL, NULL, NULL) < 0) {
		ERROR("Error getting go-ahead from server: %s", strerror(errno));
		goto out;
	}
	ret = read(sv[0], buf, 1);
out:
	close(sv[0]);
	close(sv[1]);
	if (ret == 1 && *buf == '1')
		return 0;
	return -1;
}

static int chown_cgroup_wrapper(void *data)
{
	struct chown_data *arg = data;

	if (setresgid(0,0,0) < 0)
		SYSERROR("Failed to setgid to 0");
	if (setresuid(0,0,0) < 0)
		SYSERROR("Failed to setuid to 0");
	if (setgroups(0, NULL) < 0)
		SYSERROR("Failed to clear groups");
	return do_chown_cgroup(arg->controller, arg->cgroup_path, arg->origuid);
}

/* Internal helper.  Must be called with the cgmanager dbus socket open */
static bool lxc_cgmanager_chmod(const char *controller,
		const char *cgroup_path, const char *file, int mode)
{
	if (cgmanager_chmod_sync(NULL, cgroup_manager, controller,
			cgroup_path, file, mode) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_chmod_sync failed: %s", nerr->message);
		nih_free(nerr);
		return false;
	}
	return true;
}

/* Internal helper.  Must be called with the cgmanager dbus socket open */
static bool chown_cgroup(const char *controller, const char *cgroup_path,
			struct lxc_conf *conf)
{
	struct chown_data data;

	if (lxc_list_empty(&conf->id_map))
		/* If there's no mapping then we don't need to chown */
		return true;

	data.controller = controller;
	data.cgroup_path = cgroup_path;
	data.origuid = geteuid();

	if (userns_exec_1(conf, chown_cgroup_wrapper, &data) < 0) {
		ERROR("Error requesting cgroup chown in new namespace");
		return false;
	}

	/* now chmod 775 the directory else the container cannot create cgroups */
	if (!lxc_cgmanager_chmod(controller, cgroup_path, "", 0775))
		return false;
	if (!lxc_cgmanager_chmod(controller, cgroup_path, "tasks", 0775))
		return false;
	if (!lxc_cgmanager_chmod(controller, cgroup_path, "cgroup.procs", 0775))
		return false;

	return true;
}

#define CG_REMOVE_RECURSIVE 1
/* Internal helper.  Must be called with the cgmanager dbus socket open */
static void cgm_remove_cgroup(const char *controller, const char *path)
{
	int existed;
	if ( cgmanager_remove_sync(NULL, cgroup_manager, controller,
				   path, CG_REMOVE_RECURSIVE, &existed) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_remove_sync failed: %s", nerr->message);
		nih_free(nerr);
		ERROR("Error removing %s:%s", controller, path);
	}
	if (existed == -1)
		INFO("cgroup removal attempt: %s:%s did not exist", controller, path);
}

static void *cgm_init(const char *name)
{
	struct cgm_data *d;

	if (!cgm_dbus_connect()) {
		ERROR("Error connecting to cgroup manager");
		return NULL;
	}
	d = malloc(sizeof(*d));
	if (!d) {
		cgm_dbus_disconnect();
		return NULL;
	}

	memset(d, 0, sizeof(*d));
	d->name = strdup(name);
	if (!d->name) {
		cgm_dbus_disconnect();
		goto err1;
	}

	/* if we are running as root, use system cgroup pattern, otherwise
	 * just create a cgroup under the current one. But also fall back to
	 * that if for some reason reading the configuration fails and no
	 * default value is available
	 */
	if (geteuid() == 0)
		d->cgroup_pattern = lxc_global_config_value("lxc.cgroup.pattern");
	if (!d->cgroup_pattern)
		d->cgroup_pattern = "%n";
	// cgm_create immediately gets called so keep the connection open
	return d;

err1:
	free(d);
	return NULL;
}

/* Called after a failed container startup */
static void cgm_destroy(void *hdata)
{
	struct cgm_data *d = hdata;
	int i;

	if (!d || !d->cgroup_path)
		return;
	if (!cgm_dbus_connect()) {
		ERROR("Error connecting to cgroup manager");
		return;
	}
	for (i = 0; i < nr_subsystems; i++)
		cgm_remove_cgroup(subsystems[i], d->cgroup_path);

	free(d->name);
	if (d->cgroup_path)
		free(d->cgroup_path);
	free(d);
	cgm_dbus_disconnect();
}

/*
 * remove all the cgroups created
 * called internally with dbus connection open
 */
static inline void cleanup_cgroups(char *path)
{
	int i;
	for (i = 0; i < nr_subsystems; i++)
		cgm_remove_cgroup(subsystems[i], path);
}

static inline bool cgm_create(void *hdata)
{
	struct cgm_data *d = hdata;
	int i, index=0, baselen, ret;
	int32_t existed;
	char result[MAXPATHLEN], *tmp, *cgroup_path;

	if (!d)
		return false;
// XXX we should send a hint to the cgmanager that when these
// cgroups become empty they should be deleted.  Requires a cgmanager
// extension

	memset(result, 0, MAXPATHLEN);
	tmp = lxc_string_replace("%n", d->name, d->cgroup_pattern);
	if (!tmp)
		goto bad;
	if (strlen(tmp) >= MAXPATHLEN) {
		free(tmp);
		goto bad;
	}
	strcpy(result, tmp);
	baselen = strlen(result);
	free(tmp);
	tmp = result;
	while (*tmp == '/')
		tmp++;
again:
	if (index == 100) { // turn this into a warn later
		ERROR("cgroup error?  100 cgroups with this name already running");
		goto bad;
	}
	if (index) {
		ret = snprintf(result+baselen, MAXPATHLEN-baselen, "-%d", index);
		if (ret < 0 || ret >= MAXPATHLEN-baselen)
			goto bad;
	}
	existed = 0;
	for (i = 0; i < nr_subsystems; i++) {
		if (!lxc_cgmanager_create(subsystems[i], tmp, &existed)) {
			ERROR("Error creating cgroup %s:%s", subsystems[i], result);
			cleanup_cgroups(tmp);
			goto bad;
		}
		if (existed == 1)
			goto next;
	}
	// success
	cgroup_path = strdup(tmp);
	if (!cgroup_path) {
		cleanup_cgroups(tmp);
		goto bad;
	}
	d->cgroup_path = cgroup_path;
	cgm_dbus_disconnect();
	return true;

next:
	cleanup_cgroups(tmp);
	index++;
	goto again;
bad:
	cgm_dbus_disconnect();
	return false;
}

/*
 * Use the cgmanager to move a task into a cgroup for a particular
 * hierarchy.
 * All the subsystems in this hierarchy are co-mounted, so we only
 * need to transition the task into one of the cgroups
 *
 * Internal helper, must be called with cgmanager dbus socket open
 */
static bool lxc_cgmanager_enter(pid_t pid, const char *controller,
		const char *cgroup_path, bool abs)
{
	int ret;

	if (abs)
		ret = cgmanager_move_pid_abs_sync(NULL, cgroup_manager,
			controller, cgroup_path, pid);
	else
		ret = cgmanager_move_pid_sync(NULL, cgroup_manager,
			controller, cgroup_path, pid);
	if (ret != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_move_pid_%ssync failed: %s",
			abs ? "abs_" : "", nerr->message);
		nih_free(nerr);
		return false;
	}
	return true;
}

/* Internal helper, must be called with cgmanager dbus socket open */
static bool do_cgm_enter(pid_t pid, const char *cgroup_path, bool abs)
{
	int i;

	for (i = 0; i < nr_subsystems; i++) {
		if (!lxc_cgmanager_enter(pid, subsystems[i], cgroup_path, abs))
			return false;
	}
	return true;
}

static inline bool cgm_enter(void *hdata, pid_t pid)
{
	struct cgm_data *d = hdata;
	bool ret = false;

	if (!cgm_dbus_connect()) {
		ERROR("Error connecting to cgroup manager");
		return false;
	}
	if (!d || !d->cgroup_path)
		goto out;
	if (do_cgm_enter(pid, d->cgroup_path, false))
		ret = true;
out:
	cgm_dbus_disconnect();
	return ret;
}

static const char *cgm_get_cgroup(void *hdata, const char *subsystem)
{
	struct cgm_data *d = hdata;

	if (!d || !d->cgroup_path)
		return NULL;
	return d->cgroup_path;
}

#if HAVE_CGMANAGER_GET_PID_CGROUP_ABS_SYNC
static inline bool abs_cgroup_supported(void) {
	return api_version >= CGM_SUPPORTS_GET_ABS;
}
#else
static inline bool abs_cgroup_supported(void) {
	return false;
}
#define cgmanager_get_pid_cgroup_abs_sync(...) -1
#endif

static char *try_get_abs_cgroup(const char *name, const char *lxcpath,
		const char *controller)
{
	char *cgroup = NULL;

	if (abs_cgroup_supported()) {
		/* get the container init pid and ask for its abs cgroup */
		pid_t pid = lxc_cmd_get_init_pid(name, lxcpath);
		if (pid < 0)
			return NULL;
		if (cgmanager_get_pid_cgroup_abs_sync(NULL, cgroup_manager,
				controller, pid, &cgroup) != 0) {
			cgroup = NULL;
			NihError *nerr;
			nerr = nih_error_get();
			nih_free(nerr);
		}
		return cgroup;
	}

	/* use the command interface to look for the cgroup */
	return lxc_cmd_get_cgroup_path(name, lxcpath, controller);
}

/*
 * nrtasks is called by the utmp helper by the container monitor.
 * cgmanager socket was closed after cgroup setup was complete, so we need
 * to reopen here.
 *
 * Return -1 on error.
 */
static int cgm_get_nrtasks(void *hdata)
{
	struct cgm_data *d = hdata;
	int32_t *pids;
	size_t pids_len;

	if (!d || !d->cgroup_path)
		return -1;

	if (!cgm_dbus_connect()) {
		ERROR("Error connecting to cgroup manager");
		return -1;
	}
	if (cgmanager_get_tasks_sync(NULL, cgroup_manager, subsystems[0],
				     d->cgroup_path, &pids, &pids_len) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_get_tasks_sync failed: %s", nerr->message);
		nih_free(nerr);
		pids_len = -1;
		goto out;
	}
	nih_free(pids);
out:
	cgm_dbus_disconnect();
	return pids_len;
}

static inline void free_abs_cgroup(char *cgroup)
{
	if (!cgroup)
		return;
	if (abs_cgroup_supported())
		nih_free(cgroup);
	else
		free(cgroup);
}

static void do_cgm_get(const char *name, const char *lxcpath, const char *filename, int outp, bool sendvalue)
{
	char *controller, *key, *cgroup = NULL, *cglast;
	int len = -1;
	int ret;
	nih_local char *result = NULL;

	controller = alloca(strlen(filename)+1);
	strcpy(controller, filename);
	key = strchr(controller, '.');
	if (!key) {
		ret = write(outp, &len, sizeof(len));
		if (ret != sizeof(len))
			WARN("Failed to warn cgm_get of error; parent may hang");
		exit(1);
	}
	*key = '\0';

	if (!cgm_dbus_connect()) {
		ERROR("Error connecting to cgroup manager");
		ret = write(outp, &len, sizeof(len));
		if (ret != sizeof(len))
			WARN("Failed to warn cgm_get of error; parent may hang");
		exit(1);
	}
	cgroup = try_get_abs_cgroup(name, lxcpath, subsystems[0]);
	if (!cgroup) {
		cgm_dbus_disconnect();
		ret = write(outp, &len, sizeof(len));
		if (ret != sizeof(len))
			WARN("Failed to warn cgm_get of error; parent may hang");
		exit(1);
	}
	cglast = strrchr(cgroup, '/');
	if (!cglast) {
		cgm_dbus_disconnect();
		free_abs_cgroup(cgroup);
		ret = write(outp, &len, sizeof(len));
		if (ret != sizeof(len))
			WARN("Failed to warn cgm_get of error; parent may hang");
		exit(1);
	}
	*cglast = '\0';
	if (!lxc_cgmanager_enter(getpid(), controller, cgroup, abs_cgroup_supported())) {
		ERROR("Failed to enter container cgroup %s:%s", controller, cgroup);
		ret = write(outp, &len, sizeof(len));
		if (ret != sizeof(len))
			WARN("Failed to warn cgm_get of error; parent may hang");
		cgm_dbus_disconnect();
		free_abs_cgroup(cgroup);
		exit(1);
	}
	if (cgmanager_get_value_sync(NULL, cgroup_manager, controller, cglast+1, filename, &result) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		nih_free(nerr);
		free_abs_cgroup(cgroup);
		cgm_dbus_disconnect();
		ret = write(outp, &len, sizeof(len));
		if (ret != sizeof(len))
			WARN("Failed to warn cgm_get of error; parent may hang");
		exit(1);
	}
	free_abs_cgroup(cgroup);
	cgm_dbus_disconnect();
	len = strlen(result);
	ret = write(outp, &len, sizeof(len));
	if (ret != sizeof(len)) {
		WARN("Failed to send length to parent");
		exit(1);
	}
	if (!len || !sendvalue) {
		exit(0);
	}
	ret = write(outp, result, len);
	if (ret < 0)
		exit(1);
	exit(0);
}

/* cgm_get is called to get container cgroup settings, not during startup */
static int cgm_get(const char *filename, char *value, size_t len, const char *name, const char *lxcpath)
{
	pid_t pid;
	int p[2], ret, newlen, readlen;

	if (pipe(p) < 0)
		return -1;
	if ((pid = fork()) < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (!pid)
		do_cgm_get(name, lxcpath, filename, p[1], len && value);
	close(p[1]);
	ret = read(p[0], &newlen, sizeof(newlen));
	if (ret != sizeof(newlen)) {
		close(p[0]);
		return -1;
	}
	if (!len || !value) {
		close(p[0]);
		return newlen;
	}
	memset(value, 0, len);
	if (newlen < 0) { // child is reporting an error
		close(p[0]);
		return -1;
	}
	if (newlen == 0) { // empty read
		close(p[0]);
		return 0;
	}
	readlen = newlen > len ? len : newlen;
	ret = read(p[0], value, readlen);
	close(p[0]);
	if (ret != readlen)
		return -1;
	if (newlen >= len) {
		value[len-1] = '\0';
		newlen = len-1;
	} else if (newlen+1 < len) {
		// cgmanager doesn't add eol to last entry
		value[newlen++] = '\n';
		value[newlen] = '\0';
	}
	return newlen;
}

static void do_cgm_set(const char *name, const char *lxcpath, const char *filename, const char *value, int outp)
{
	char *controller, *key, *cgroup = NULL;
	int retval = 0;  // value we are sending to the parent over outp
	int ret;
	char *cglast;

	controller = alloca(strlen(filename)+1);
	strcpy(controller, filename);
	key = strchr(controller, '.');
	if (!key) {
		ret = write(outp, &retval, sizeof(retval));
		if (ret != sizeof(retval))
			WARN("Failed to warn cgm_set of error; parent may hang");
		exit(1);
	}
	*key = '\0';

	if (!cgm_dbus_connect()) {
		ERROR("Error connecting to cgroup manager");
		ret = write(outp, &retval, sizeof(retval));
		if (ret != sizeof(retval))
			WARN("Failed to warn cgm_set of error; parent may hang");
		exit(1);
	}
	cgroup = try_get_abs_cgroup(name, lxcpath, subsystems[0]);
	if (!cgroup) {
		cgm_dbus_disconnect();
		ret = write(outp, &retval, sizeof(retval));
		if (ret != sizeof(retval))
			WARN("Failed to warn cgm_set of error; parent may hang");
		exit(1);
	}
	cglast = strrchr(cgroup, '/');
	if (!cglast) {
		cgm_dbus_disconnect();
		free_abs_cgroup(cgroup);
		ret = write(outp, &retval, sizeof(retval));
		if (ret != sizeof(retval))
			WARN("Failed to warn cgm_set of error; parent may hang");
		exit(1);
	}
	*cglast = '\0';
	if (!lxc_cgmanager_enter(getpid(), controller, cgroup, abs_cgroup_supported())) {
		ERROR("Failed to enter container cgroup %s:%s", controller, cgroup);
		ret = write(outp, &retval, sizeof(retval));
		if (ret != sizeof(retval))
			WARN("Failed to warn cgm_set of error; parent may hang");
		cgm_dbus_disconnect();
		free_abs_cgroup(cgroup);
		exit(1);
	}
	if (cgmanager_set_value_sync(NULL, cgroup_manager, controller, cglast+1, filename, value) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("Error setting cgroup value %s for %s:%s", filename, controller, cgroup);
		ERROR("call to cgmanager_set_value_sync failed: %s", nerr->message);
		nih_free(nerr);
		free_abs_cgroup(cgroup);
		cgm_dbus_disconnect();
		ret = write(outp, &retval, sizeof(retval));
		if (ret != sizeof(retval))
			WARN("Failed to warn cgm_set of error; parent may hang");
		exit(1);
	}
	free_abs_cgroup(cgroup);
	cgm_dbus_disconnect();
	/* tell parent that we are done */
	retval = 1;
	ret = write(outp, &retval, sizeof(retval));
	if (ret != sizeof(retval)) {
		exit(1);
	}
	exit(0);
}

/* cgm_set is called to change cgroup settings, not during startup */
static int cgm_set(const char *filename, const char *value, const char *name, const char *lxcpath)
{
	pid_t pid;
	int p[2], ret, v;

	if (pipe(p) < 0)
		return -1;
	if ((pid = fork()) < 0) {
		close(p[1]);
		close(p[0]);
		return -1;
	}
	if (!pid)
		do_cgm_set(name, lxcpath, filename, value, p[1]);
	close(p[1]);
	ret = read(p[0], &v, sizeof(v));
	close(p[0]);
	if (ret != sizeof(v) || !v)
		return -1;
	return 0;
}

static void free_subsystems(void)
{
	int i;

	for (i = 0; i < nr_subsystems; i++)
		free(subsystems[i]);
	free(subsystems);
	subsystems = NULL;
	nr_subsystems = 0;
}

static void cull_user_controllers(void)
{
	int i, j;

	for (i = 0;  i < nr_subsystems; i++) {
		if (strncmp(subsystems[i], "name=", 5) != 0)
			continue;
		for (j = i;  j < nr_subsystems-1; j++)
			subsystems[j] = subsystems[j+1];
		nr_subsystems--;
	}
}

static bool collect_subsytems(void)
{
	char *line = NULL;
	size_t sz = 0;
	FILE *f;

	if (subsystems) // already initialized
		return true;

	f = fopen_cloexec("/proc/self/cgroup", "r");
	if (!f) {
		f = fopen_cloexec("/proc/1/cgroup", "r");
		if (!f)
			return false;
	}
	while (getline(&line, &sz, f) != -1) {
		/* file format: hierarchy:subsystems:group,
		 * with multiple subsystems being ,-separated */
		char *slist, *end, *p, *saveptr = NULL, **tmp;

		if (!line[0])
			continue;

		slist = strchr(line, ':');
		if (!slist)
			continue;
		slist++;
		end = strchr(slist, ':');
		if (!end)
			continue;
		*end = '\0';

		for (p = strtok_r(slist, ",", &saveptr);
				p;
				p = strtok_r(NULL, ",", &saveptr)) {
			tmp = realloc(subsystems, (nr_subsystems+1)*sizeof(char *));
			if (!tmp)
				goto out_free;

			subsystems = tmp;
			tmp[nr_subsystems] = strdup(p);
			if (!tmp[nr_subsystems])
				goto out_free;
			nr_subsystems++;
		}
	}
	fclose(f);

	free(line);
	if (!nr_subsystems) {
		ERROR("No cgroup subsystems found");
		return false;
	}

	return true;

out_free:
	free(line);
	fclose(f);
	free_subsystems();
	return false;
}

/*
 * called during cgroup.c:cgroup_ops_init(), at startup.  No threads.
 * We check whether we can talk to cgmanager, escape to root cgroup if
 * we are root, then close the connection.
 */
struct cgroup_ops *cgm_ops_init(void)
{
	if (!collect_subsytems())
		return NULL;
	if (!cgm_dbus_connect())
		goto err1;

	// root;  try to escape to root cgroup
	if (geteuid() == 0 && !lxc_cgmanager_escape())
		goto err2;
	cgm_dbus_disconnect();

	return &cgmanager_ops;

err2:
	cgm_dbus_disconnect();
err1:
	free_subsystems();
	return NULL;
}

/* unfreeze is called by the command api after killing a container.  */
static bool cgm_unfreeze(void *hdata)
{
	struct cgm_data *d = hdata;
	bool ret = true;

	if (!d || !d->cgroup_path)
		return false;

	if (!cgm_dbus_connect()) {
		ERROR("Error connecting to cgroup manager");
		return false;
	}
	if (cgmanager_set_value_sync(NULL, cgroup_manager, "freezer", d->cgroup_path,
			"freezer.state", "THAWED") != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_set_value_sync failed: %s", nerr->message);
		nih_free(nerr);
		ERROR("Error unfreezing %s", d->cgroup_path);
		ret = false;
	}
	cgm_dbus_disconnect();
	return ret;
}

static bool cgm_setup_limits(void *hdata, struct lxc_list *cgroup_settings, bool do_devices)
{
	struct cgm_data *d = hdata;
	struct lxc_list *iterator;
	struct lxc_cgroup *cg;
	bool ret = false;

	if (lxc_list_empty(cgroup_settings))
		return true;

	if (!d || !d->cgroup_path)
		return false;

	if (!cgm_dbus_connect()) {
		ERROR("Error connecting to cgroup manager");
		return false;
	}

	lxc_list_for_each(iterator, cgroup_settings) {
		char controller[100], *p;
		cg = iterator->elem;
		if (do_devices != !strncmp("devices", cg->subsystem, 7))
			continue;
		if (strlen(cg->subsystem) > 100) // i smell a rat
			goto out;
		strcpy(controller, cg->subsystem);
		p = strchr(controller, '.');
		if (p)
			*p = '\0';
		if (cgmanager_set_value_sync(NULL, cgroup_manager, controller,
					 d->cgroup_path, cg->subsystem, cg->value) != 0) {
			NihError *nerr;
			nerr = nih_error_get();
			ERROR("call to cgmanager_set_value_sync failed: %s", nerr->message);
			nih_free(nerr);
			ERROR("Error setting cgroup %s:%s limit type %s", controller,
				d->cgroup_path, cg->subsystem);
			goto out;
		}

		DEBUG("cgroup '%s' set to '%s'", cg->subsystem, cg->value);
	}

	ret = true;
	INFO("cgroup limits have been setup");
out:
	cgm_dbus_disconnect();
	return ret;
}

static bool cgm_chown(void *hdata, struct lxc_conf *conf)
{
	struct cgm_data *d = hdata;
	int i;

	if (!d || !d->cgroup_path)
		return false;
	if (!cgm_dbus_connect()) {
		ERROR("Error connecting to cgroup manager");
		return false;
	}
	for (i = 0; i < nr_subsystems; i++) {
		if (!chown_cgroup(subsystems[i], d->cgroup_path, conf))
			WARN("Failed to chown %s:%s to container root",
				subsystems[i], d->cgroup_path);
	}
	cgm_dbus_disconnect();
	return true;
}

/*
 * TODO: this should be re-written to use the get_config_item("lxc.id_map")
 * cmd api instead of getting the idmap from c->lxc_conf.  The reason is
 * that the id_maps may be different if the container was started with a
 * -f or -s argument.
 * The reason I'm punting on that is because we'll need to parse the
 * idmap results.
 */
static bool cgm_attach(const char *name, const char *lxcpath, pid_t pid)
{
	bool pass;
	char *cgroup = NULL;

	if (!cgm_dbus_connect()) {
		ERROR("Error connecting to cgroup manager");
		return false;
	}
	// cgm_create makes sure that we have the same cgroup name for all
	// subsystems, so since this is a slow command over the cmd socket,
	// just get the cgroup name for the first one.
	cgroup = try_get_abs_cgroup(name, lxcpath, subsystems[0]);
	if (!cgroup) {
		ERROR("Failed to get cgroup for controller %s", subsystems[0]);
		cgm_dbus_disconnect();
		return false;
	}

	pass = do_cgm_enter(pid, cgroup, abs_cgroup_supported());
	cgm_dbus_disconnect();
	if (!pass)
		ERROR("Failed to enter group %s", cgroup);

	free_abs_cgroup(cgroup);
	return pass;
}

static bool cgm_bind_dir(const char *root, const char *dirname)
{
	nih_local char *cgpath = NULL;

	/* /sys should have been mounted by now */
	cgpath = NIH_MUST( nih_strdup(NULL, root) );
	NIH_MUST( nih_strcat(&cgpath, NULL, "/sys/fs/cgroup") );

	if (!dir_exists(cgpath)) {
		ERROR("%s does not exist", cgpath);
		return false;
	}

	/* mount a tmpfs there so we can create subdirs */
	if (mount("cgroup", cgpath, "tmpfs", 0, "size=10000,mode=755")) {
		SYSERROR("Failed to mount tmpfs at %s", cgpath);
		return false;
	}
	NIH_MUST( nih_strcat(&cgpath, NULL, "/cgmanager") );

	if (mkdir(cgpath, 0755) < 0) {
		SYSERROR("Failed to create %s", cgpath);
		return false;
	}

	if (mount(dirname, cgpath, "none", MS_BIND, 0)) {
		SYSERROR("Failed to bind mount %s to %s", dirname, cgpath);
		return false;
	}

	return true;
}

/*
 * cgm_mount_cgroup:
 * If /sys/fs/cgroup/cgmanager.lower/ exists, bind mount that to
 * /sys/fs/cgroup/cgmanager/ in the container.
 * Otherwise, if /sys/fs/cgroup/cgmanager exists, bind mount that.
 * Else do nothing
 */
#define CGMANAGER_LOWER_SOCK "/sys/fs/cgroup/cgmanager.lower"
#define CGMANAGER_UPPER_SOCK "/sys/fs/cgroup/cgmanager"
static bool cgm_mount_cgroup(void *hdata, const char *root, int type)
{
	if (dir_exists(CGMANAGER_LOWER_SOCK))
		return cgm_bind_dir(root, CGMANAGER_LOWER_SOCK);
	if (dir_exists(CGMANAGER_UPPER_SOCK))
		return cgm_bind_dir(root, CGMANAGER_UPPER_SOCK);
	// Host doesn't have cgmanager running?  Then how did we get here?
	return false;
}

static struct cgroup_ops cgmanager_ops = {
	.init = cgm_init,
	.destroy = cgm_destroy,
	.create = cgm_create,
	.enter = cgm_enter,
	.create_legacy = NULL,
	.get_cgroup = cgm_get_cgroup,
	.get = cgm_get,
	.set = cgm_set,
	.unfreeze = cgm_unfreeze,
	.setup_limits = cgm_setup_limits,
	.name = "cgmanager",
	.chown = cgm_chown,
	.attach = cgm_attach,
	.mount_cgroup = cgm_mount_cgroup,
	.nrtasks = cgm_get_nrtasks,
	.disconnect = NULL,
};
#endif
