/* cgmanager-proxy
 *
 * Copyright © 2013 Stephane Graber
 * Author: Stephane Graber <stgraber@ubuntu.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <frontend.h>

DBusConnection *server_conn;

bool master_running(void)
{
	NihError *err;

	/* is manager already running under cgmanager.lower */
	server_conn = nih_dbus_connect(CGPROXY_DBUS_PATH, NULL);
	if (server_conn) {
		dbus_connection_unref (server_conn);
		return true;
	}
	err = nih_error_get();
	nih_free(err);

	/* is manager running under cgmanager */
	server_conn = nih_dbus_connect(CGMANAGER_DBUS_PATH, NULL);
	if (server_conn) {
		dbus_connection_unref (server_conn);
		return true;
	}
	err = nih_error_get();
	nih_free(err);
	return false;
}

static void cgm_dbus_disconnected(DBusConnection *connection);

int setup_proxy(void)
{
	bool exists_upper = false, exists_lower = false;
	NihError *err;

	/*
	 * If /sys/fs/cgroup/cgmanager.lower exists,
	 *    if /sys/fs/cgroup/cgmanager exists, then exit (proxy already running)
	 *    start up, connect to .lower
	 * else
	 *    if /sys/fs/cgroup/cgmanager exists, move it to /sys/fs/cgroup/cgmanager.lower
	 *    start up and connect to .lower
	 */
	server_conn = nih_dbus_connect(CGMANAGER_DBUS_PATH, NULL);
	if (server_conn) {
		exists_upper = true;
		dbus_connection_unref (server_conn);
	} else {
		err = nih_error_get();
		nih_free(err);
	}
	server_conn = nih_dbus_connect(CGPROXY_DBUS_PATH, cgm_dbus_disconnected);
	if (server_conn) {
		exists_lower = true;
	} else {
		err = nih_error_get();
		nih_free(err);
	}
	if (exists_upper && exists_lower) {
		dbus_connection_unref (server_conn);
		nih_fatal("proxy already running");
		return -1;  // proxy already running
	}
	if (exists_lower)
		// we've got the sock we need, all set.
		return 0;
	if (exists_upper) {
		//move /sys/fs/cgroup/cgmanager to /sys/fs/cgroup/cgmanager.lower
		if (mkdir(CGPROXY_DIR, 0755) < 0 && errno != EEXIST) {
			nih_fatal("failed to create lower sock");
			return -1;
		}
		if (mount(CGMANAGER_DIR, CGPROXY_DIR, "none", MS_MOVE, 0) < 0) {
			/* it wasn't a mount, meaning we are at the host
			 * level on an old kernel.  So rename it */
			if (unlink(CGPROXY_SOCK) && errno != ENOENT)
				nih_warn("failed to remove %s: %s", CGPROXY_SOCK,
					strerror(errno));
			if (rmdir(CGPROXY_DIR) && errno != ENOENT)
				nih_warn("failed to remove %s: %s", CGPROXY_DIR,
					strerror(errno));
			if (rename(CGMANAGER_DIR, CGPROXY_DIR) < 0) {
				nih_fatal("unable to rename the socket");
				return -1;
			}
			if (mkdir(CGMANAGER_DIR, 0755) < 0) {
				nih_fatal("unable to create socket dir");
				return -1;
			}
		}
	}
	server_conn = nih_dbus_connect(CGPROXY_DBUS_PATH, cgm_dbus_disconnected);
	if (!server_conn) {
		err = nih_error_get();
		nih_fatal("Failed to open connection to %s: %s",
				CGPROXY_DBUS_PATH, err->message);
		nih_free(err);

		return -1;
	}
	return 0;
}

static void cgm_dbus_disconnected(DBusConnection *connection)
{
	NihError *err;

	dbus_connection_unref(connection);
	while (1) {
		server_conn = nih_dbus_connect(CGPROXY_DBUS_PATH, cgm_dbus_disconnected);
		if (server_conn)
			return;
		err = nih_error_get();
		nih_error("Failed to re-open connection to %s: %s",
				CGPROXY_DBUS_PATH, err->message);
		nih_free(err);
		nih_error("re-trying in 5 seconds\n");
		sleep(5);
	}
}

static int checkmaster = FALSE;

bool send_dummy_msg(DBusConnection *conn)
{
	DBusMessage *message = NULL;
	DBusMessageIter iter;
    int a;

	message = dbus_message_new_method_call(dbus_bus_get_unique_name(conn),
			"/org/linuxcontainers/cgmanager",
			"org.linuxcontainers.cgmanager0_0", "Ping");
	if (!message) {
		nih_error("%s: failed to create ping message", __func__);
		return false;
	}
	dbus_message_set_no_reply(message, TRUE);
	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &a)) {
		dbus_message_unref(message);
		nih_error("%s: out of memory", __func__);
		return false;
	}
	dbus_connection_send(conn, message, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(message);
	return true;
}

static DBusMessage *start_dbus_request(const char *method, int *sv)
{
	int optval = 1;
	DBusMessage *msg = NULL;

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
		nih_error("%s: Error creating socketpair: %s",
			__func__, strerror(errno));
		return NULL;
	}
	if (setsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		nih_error("%s: setsockopt: %s", __func__, strerror(errno));
		goto err;
	}
	if (setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		nih_error("%s: setsockopt: %s", __func__, strerror(errno));
		goto err;
	}

	msg = dbus_message_new_method_call(dbus_bus_get_unique_name(server_conn),
			"/org/linuxcontainers/cgmanager",
			"org.linuxcontainers.cgmanager0_0", method);
	if (msg)
		return msg;

err:
	close(sv[0]);
	close(sv[1]);
	return NULL;
}

static bool complete_dbus_request(DBusMessage *message,
		int *sv, struct ucred *rcred, struct ucred *vcred)
{
	char buf[1];

	if (!dbus_connection_send(server_conn, message, NULL)) {
		nih_error("%s: failed to send dbus message", __func__);
		dbus_message_unref(message);
		return false;
	}
	dbus_connection_flush(server_conn);
	dbus_message_unref(message);

	if (recv(sv[0], buf, 1, 0) != 1) {
		nih_error("%s: Error getting reply from server over socketpair",
			  __func__);
		return false;
	}
	if (send_creds(sv[0], rcred)) {
		nih_error("%s: Error sending pid over SCM_CREDENTIAL",
			__func__);
		return false;
	}

	if (!vcred) // this request only requires one scm_credential
		return true;

	if (recv(sv[0], buf, 1, 0) != 1) {
		nih_error("%s: Error getting reply from server over socketpair",
			__func__);
		return false;
	}
	if (send_creds(sv[0], vcred)) {
		nih_error("%s: Error sending pid over SCM_CREDENTIAL",
			__func__);
		return false;
	}

	return true;
}

int get_pid_cgroup_main (void *parent, const char *controller,
		struct ucred p, struct ucred r, struct ucred v, char **output)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char s[MAXPATHLEN] = { 0 };

	if (memcmp(&p, &r, sizeof(struct ucred)) != 0) {
		nih_error("%s: proxy != requestor", __func__);
		return -1;
	}

	if (!(message = start_dbus_request("GetPidCgroupScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (!dbus_message_iter_append_basic (&iter,
			DBUS_TYPE_STRING,
			&controller)) {
		dbus_message_unref(message);
		nih_error("%s: out of memory", __func__);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		dbus_message_unref(message);
		nih_error("%s: out of memory", __func__);
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, &v)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	// TODO - switch to nih_io_message_recv?
	if (recv(sv[0], s, MAXPATHLEN-1, 0) <= 0)
		nih_error("%s: Error reading result from cgmanager",
			__func__);
	else {
		*output = NIH_MUST( nih_strdup(parent, s) );
		ret = 0;
	}
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int do_move_pid_main (const char *controller, const char *cgroup,
		struct ucred p, struct ucred r, struct ucred v,
		const char *cmd)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (memcmp(&p, &r, sizeof(struct ucred)) != 0) {
		nih_error("%s: proxy != requestor", __func__);
		return -1;
	}

	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}

	if (!(message = start_dbus_request(cmd, sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,
				&controller)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, &v)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && *buf == '1')
		ret = 0;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int move_pid_main (const char *controller, const char *cgroup,
		struct ucred p, struct ucred r, struct ucred v)
{
	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}
	if (cgroup[0] == '/') {
		nih_error("%s: uid %u tried to escape its cgroup", __func__, r.uid);
		return -1;
	}

	return do_move_pid_main(controller, cgroup, p, r, v, "MovePidScm");
}

int move_pid_abs_main (const char *controller, const char *cgroup,
		struct ucred p, struct ucred r, struct ucred v)
{
	if (r.uid) {
		nih_error("%s: uid %u tried to escape", __func__, r.uid);
		return -1;
	}
	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}
	return do_move_pid_main(controller, cgroup, p, r, v, "MovePidAbsScm");
}

int create_main (const char *controller, const char *cgroup, struct ucred p,
		struct ucred r, int32_t *existed)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (memcmp(&p, &r, sizeof(struct ucred)) != 0) {
		nih_error("%s: proxy != requestor", __func__);
		return -1;
	}

	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}

	if (!(message = start_dbus_request("CreateScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && (*buf == '1' || *buf == '2'))
		ret = 0;
	*existed = *buf == '2' ? 1 : -1;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int chown_main (const char *controller, const char *cgroup,
		struct ucred p, struct ucred r, struct ucred v)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (memcmp(&p, &r, sizeof(struct ucred)) != 0) {
		nih_error("%s: proxy != requestor", __func__);
		return -1;
	}

	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}

	if (!(message = start_dbus_request("ChownScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, &v)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && *buf == '1')
		ret = 0;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int chmod_main (const char *controller, const char *cgroup, const char *file,
		struct ucred p, struct ucred r, int mode)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (memcmp(&p, &r, sizeof(struct ucred)) != 0) {
		nih_error("%s: proxy != requestor", __func__);
		return -1;
	}

	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}

	if (!(message = start_dbus_request("ChmodScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &file)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &mode)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && *buf == '1')
		ret = 0;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int get_value_main (void *parent, const char *controller, const char *cgroup,
		 const char *key, struct ucred p, struct ucred r, char **value)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char output[MAXPATHLEN] = { 0 };

	if (memcmp(&p, &r, sizeof(struct ucred)) != 0) {
		nih_error("%s: proxy != requestor", __func__);
		return -1;
	}

	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}

	if (!(message = start_dbus_request("GetValueScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], output, MAXPATHLEN, 0) <= 0) {
		nih_error("%s: Failed reading string from cgmanager: %s",
			__func__, strerror(errno));
	} else {
		*value = NIH_MUST( nih_strdup(parent, output) );
		ret = 0;
	}
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int set_value_main (const char *controller, const char *cgroup,
		 const char *key, const char *value, struct ucred p,
		 struct ucred r)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (memcmp(&p, &r, sizeof(struct ucred)) != 0) {
		nih_error("%s: proxy != requestor", __func__);
		return -1;
	}

	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}

	if (!(message = start_dbus_request("SetValueScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &value)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && *buf == '1')
		ret = 0;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int remove_main (const char *controller, const char *cgroup, struct ucred p,
		struct ucred r, int recursive, int32_t *existed)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (memcmp(&p, &r, sizeof(struct ucred)) != 0) {
		nih_error("%s: proxy != requestor", __func__);
		return -1;
	}

	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}

	if (!(message = start_dbus_request("RemoveScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &recursive)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && (*buf == '1' || *buf == '2'))
		ret = 0;
	*existed = *buf == '2' ? 1 : -1;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int get_tasks_main (void *parent, const char *controller, const char *cgroup,
		    struct ucred p, struct ucred r, int32_t **pids)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	uint32_t nrpids;
	struct ucred tcred;
	int i;

	if (memcmp(&p, &r, sizeof(struct ucred)) != 0) {
		nih_error("%s: proxy != requestor", __func__);
		return -1;
	}

	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}

	if (!(message = start_dbus_request("GetTasksScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}
	if (recv(sv[0], &nrpids, sizeof(uint32_t), 0) != sizeof(uint32_t))
		goto out;
	if (nrpids == -1) {
		nih_error("%s: bad cgroup: %s:%s", __func__, controller, cgroup);
		ret = -1;
		goto out;
	}
	if (nrpids == 0) {
		ret = 0;
		goto out;
	}

	*pids = NIH_MUST( nih_alloc(parent, nrpids * sizeof(uint32_t)) );
	for (i=0; i<nrpids; i++) {
		get_scm_creds_sync(sv[0], &tcred);
		if (tcred.pid == -1) {
			nih_error("%s: Failed getting pid from server",
				__func__);
			goto out;
		}
		(*pids)[i] = tcred.pid;
	}
	ret = nrpids;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int list_children_main (void *parent, const char *controller, const char *cgroup,
		    struct ucred p, struct ucred r, char ***output)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	uint32_t len;
	int32_t nrkids;
	nih_local char * paths = NULL;
	char *s;
	int i;

	*output = NULL;
	if (memcmp(&p, &r, sizeof(struct ucred)) != 0) {
		nih_error("%s: proxy != requestor", __func__);
		return -1;
	}

	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}

	if (!(message = start_dbus_request("ListChildrenScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], &nrkids, sizeof(int32_t), 0) != sizeof(int32_t))
		goto out;
	if (nrkids == 0) {
		ret = 0;
		goto out;
	}
	if (nrkids < 0) {
		nih_error("%s: Server encountered an error: bad cgroup?", __func__);
		ret = -1;
		goto out;
	}
	if (recv(sv[0], &len, sizeof(uint32_t), 0) != sizeof(uint32_t))
		goto out;

	paths = nih_alloc(NULL, len+1);
	paths[len] = '\0';
	if (read(sv[0], paths, len) != len) {
		nih_error("%s: Failed getting paths from server", __func__);
		goto out;
	}

	*output = NIH_MUST( nih_alloc(parent, sizeof( char*)*(nrkids+1)) );

	s = paths;
	(*output)[nrkids] = NULL;
	for (i=0; i<nrkids; i++) {
		if (s > paths + len) {
			ret = -1;
			nih_error("%s: corrupted result from cgmanager",
					__func__);
			goto out;
		}
		(*output)[i] = NIH_MUST( nih_strdup(parent, s) );
		s += strlen(s) + 1;
	}
	ret = nrkids;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int remove_on_empty_main (const char *controller, const char *cgroup,
		struct ucred p, struct ucred r)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (memcmp(&p, &r, sizeof(struct ucred)) != 0) {
		nih_error("%s: proxy != requestor", __func__);
		return -1;
	}

	if (!sane_cgroup(cgroup)) {
		nih_error("%s: unsafe cgroup", __func__);
		return -1;
	}

	if (!(message = start_dbus_request("RemoveOnEmptyScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error("%s: out of memory", __func__);
		dbus_message_unref(message);
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && (*buf == '1'))
		ret = 0;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}


/**
 * options:
 *
 * Command-line options accepted by this program.
 **/
static NihOption options[] = {
	{ 0, "daemon", N_("Detach and run in the background"),
	  NULL, NULL, &daemonise, NULL },
	{ 0, "sigstop", N_("Raise SIGSTOP when ready"),
		NULL, NULL, &sigstop, NULL },
	{ 0, "check-master", N_("Check whether cgmanager is running"),
	  NULL, NULL, &checkmaster, NULL },

	NIH_OPTION_LAST
};

int
main (int argc, char *argv[])
{
	char **		args;
	int		ret;
	DBusServer *	server;
	struct stat sb;

	nih_main_init (argv[0]);

	nih_option_set_synopsis (_("Control group proxy"));
	nih_option_set_help (_("The cgroup manager proxy"));

	args = nih_option_parser (NULL, argc, argv, options, FALSE);
	if (! args)
		exit (1);

	if (geteuid() != 0) {
		nih_error("%s: Cgmanager proxy must be run as root", __func__);
		exit(1);
	}

	/*
	 * If we are called with checkmaster, then only check whether
	 * cgmanager is running.  This is used by the init script to
	 * determine whether to run cgmanager or cgproxy
	 */
	if (checkmaster) {
		if (master_running())
			exit(0);
		exit(1);
	}

	connection_timeout_init();

	if (setup_proxy() < 0) {
		nih_fatal ("Failed to set up as proxy");
		exit(1);
	}

	/* Setup the DBus server */
	server = nih_dbus_server ( CGMANAGER_DBUS_PATH, client_connect,
			client_disconnect);
	nih_assert (server != NULL);

	if (stat("/proc/self/ns/pid", &sb) == 0) {
		mypidns = read_pid_ns_link(getpid());
		setns_pid_supported = true;
	}

	if (stat("/proc/self/ns/user", &sb) == 0) {
		myuserns = read_user_ns_link(getpid());
		setns_user_supported = true;
	}

	/* Become daemon */
	if (daemonise) {
		if (nih_main_daemonise () < 0) {
			NihError *err;

			err = nih_error_get ();
			nih_fatal ("%s: %s", _("Unable to become daemon"),
				   err->message);
			nih_free (err);

			exit (1);
		}
	}

	/*
	 * We have to send a message to force fd passing over the dbus
	 * link to be negotiated.  Else when we try to attach an fd we'll
	 * fail.
	 */
	if (!send_dummy_msg(server_conn)) {
		nih_fatal("Failed to send opening ping to cgmanager");
		exit(1);
	}

	if (sigstop)
		raise(SIGSTOP);

	ret = nih_main_loop ();

	/* Destroy any PID file we may have created */
	if (daemonise) {
		nih_main_unlink_pidfile();
	}

	return ret;
}
