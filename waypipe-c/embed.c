/*
 * Copyright © 2023 the gdwaypipe_c authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Embedded-mode machinery for waypipe: everything fork-local to the
 * gdwaypipe_c fork that lets waypipe run in-process inside a host process.
 *
 * - embedded no-fork mode state + the 50 ms poll-timeout cap;
 * - owner-key shutdown registry (per-session teardown scoping);
 * - owned-child registry (per-session child reaping);
 * - the in-process SSH launcher hook (worker thread in embedded mode, forked
 *   child otherwise, posix_spawnp fallback);
 * - the in-process runner exit-code pipe helpers;
 * - the embedded worker-thread path for new client connections.
 */

#include "embed.h"

#include "main.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* envp is nonstandard, so use environ */
extern char **environ;

/* ---- embedded-mode state ---- */

waypipe_log_sink_func_t waypipe_log_sink = NULL;
static volatile sig_atomic_t g_waypipe_embedded_no_fork_mode = 0;

void waypipe_set_embedded_no_fork_mode(bool enabled)
{
	g_waypipe_embedded_no_fork_mode = enabled ? 1 : 0;
}

bool waypipe_get_embedded_no_fork_mode(void)
{
	return g_waypipe_embedded_no_fork_mode != 0;
}

int waypipe_poll_timeout_ms(int requested_timeout_ms)
{
	if (requested_timeout_ms >= 0) {
		return requested_timeout_ms;
	}

	if (waypipe_get_embedded_no_fork_mode()) {
		/* In embedded mode, do not block forever so loops can re-check
		 * shutdown_flag and teardown state in bounded time. */
		return 50;
	}

	return requested_timeout_ms;
}

/* ---- owner-key shutdown registry ---- */

static pthread_mutex_t g_waypipe_owner_shutdown_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *g_waypipe_owner_shutdown_keys = NULL;
static int g_waypipe_owner_shutdown_count = 0;
static int g_waypipe_owner_shutdown_cap = 0;

static uint64_t thread_owner_key_from_pthread(pthread_t tid)
{
	uint64_t key = 0;
	size_t copy_len = sizeof(key) < sizeof(tid) ? sizeof(key) : sizeof(tid);
	memcpy(&key, &tid, copy_len);
	return key;
}

uint64_t waypipe_get_current_owner_key(void)
{
	return thread_owner_key_from_pthread(pthread_self());
}

void waypipe_request_owner_shutdown(uint64_t owner_key)
{
	if (owner_key == 0) {
		return;
	}

	pthread_mutex_lock(&g_waypipe_owner_shutdown_lock);
	for (int i = 0; i < g_waypipe_owner_shutdown_count; i++) {
		if (g_waypipe_owner_shutdown_keys[i] == owner_key) {
			pthread_mutex_unlock(&g_waypipe_owner_shutdown_lock);
			return;
		}
	}

	if (g_waypipe_owner_shutdown_count >= g_waypipe_owner_shutdown_cap) {
		int new_cap = g_waypipe_owner_shutdown_cap > 0 ?
				g_waypipe_owner_shutdown_cap * 2 :
				8;
		uint64_t *new_buf = realloc(g_waypipe_owner_shutdown_keys,
				sizeof(uint64_t) * (size_t)new_cap);
		if (!new_buf) {
			pthread_mutex_unlock(&g_waypipe_owner_shutdown_lock);
			return;
		}
		g_waypipe_owner_shutdown_keys = new_buf;
		g_waypipe_owner_shutdown_cap = new_cap;
	}

	g_waypipe_owner_shutdown_keys[g_waypipe_owner_shutdown_count++] = owner_key;
	pthread_mutex_unlock(&g_waypipe_owner_shutdown_lock);
}

void waypipe_clear_owner_shutdown(uint64_t owner_key)
{
	if (owner_key == 0) {
		return;
	}

	pthread_mutex_lock(&g_waypipe_owner_shutdown_lock);
	for (int i = 0; i < g_waypipe_owner_shutdown_count; i++) {
		if (g_waypipe_owner_shutdown_keys[i] != owner_key) {
			continue;
		}

		if (i != g_waypipe_owner_shutdown_count - 1) {
			memmove(&g_waypipe_owner_shutdown_keys[i],
					&g_waypipe_owner_shutdown_keys[i + 1],
					sizeof(uint64_t) * (size_t)(g_waypipe_owner_shutdown_count - i - 1));
		}
		g_waypipe_owner_shutdown_count--;
		break;
	}
	pthread_mutex_unlock(&g_waypipe_owner_shutdown_lock);
}

bool waypipe_owner_shutdown_requested(uint64_t owner_key)
{
	if (owner_key == 0) {
		return false;
	}

	bool requested = false;
	pthread_mutex_lock(&g_waypipe_owner_shutdown_lock);
	for (int i = 0; i < g_waypipe_owner_shutdown_count; i++) {
		if (g_waypipe_owner_shutdown_keys[i] == owner_key) {
			requested = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_waypipe_owner_shutdown_lock);
	return requested;
}

bool waypipe_owner_shutdown_requested_current_thread(void)
{
	return waypipe_owner_shutdown_requested(waypipe_get_current_owner_key());
}

/* ---- owned-child registry ---- */

struct waypipe_owned_child_entry {
	uint64_t owner_key;
	pid_t pid;
};

static pthread_mutex_t g_waypipe_owned_children_lock = PTHREAD_MUTEX_INITIALIZER;
static struct waypipe_owned_child_entry *g_waypipe_owned_children = NULL;
static int g_waypipe_owned_children_count = 0;
static int g_waypipe_owned_children_cap = 0;

void waypipe_register_owned_child_pid(uint64_t owner_key, pid_t pid)
{
	if (owner_key == 0 || pid <= 0) {
		return;
	}

	pthread_mutex_lock(&g_waypipe_owned_children_lock);
	for (int i = 0; i < g_waypipe_owned_children_count; i++) {
		if (g_waypipe_owned_children[i].pid == pid) {
			g_waypipe_owned_children[i].owner_key = owner_key;
			pthread_mutex_unlock(&g_waypipe_owned_children_lock);
			return;
		}
	}

	if (g_waypipe_owned_children_count >= g_waypipe_owned_children_cap) {
		int new_cap = g_waypipe_owned_children_cap > 0 ?
				g_waypipe_owned_children_cap * 2 :
				8;
		struct waypipe_owned_child_entry *new_buf = realloc(
				g_waypipe_owned_children,
				sizeof(struct waypipe_owned_child_entry) * (size_t)new_cap);
		if (!new_buf) {
			pthread_mutex_unlock(&g_waypipe_owned_children_lock);
			return;
		}
		g_waypipe_owned_children = new_buf;
		g_waypipe_owned_children_cap = new_cap;
	}

	g_waypipe_owned_children[g_waypipe_owned_children_count++] =
			(struct waypipe_owned_child_entry){
					.owner_key = owner_key,
					.pid = pid,
			};
	pthread_mutex_unlock(&g_waypipe_owned_children_lock);
}

void waypipe_unregister_owned_child_pid(pid_t pid)
{
	if (pid <= 0) {
		return;
	}

	pthread_mutex_lock(&g_waypipe_owned_children_lock);
	for (int i = 0; i < g_waypipe_owned_children_count; i++) {
		if (g_waypipe_owned_children[i].pid != pid) {
			continue;
		}

		if (i != g_waypipe_owned_children_count - 1) {
			memmove(&g_waypipe_owned_children[i],
					&g_waypipe_owned_children[i + 1],
					sizeof(struct waypipe_owned_child_entry) *
							(size_t)(g_waypipe_owned_children_count - i - 1));
		}
		g_waypipe_owned_children_count--;
		break;
	}
	pthread_mutex_unlock(&g_waypipe_owned_children_lock);
}

void waypipe_force_terminate_owned_children(uint64_t owner_key)
{
	if (owner_key == 0) {
		return;
	}

	pid_t pids[64];
	int pid_count = 0;

	pthread_mutex_lock(&g_waypipe_owned_children_lock);
	for (int i = 0; i < g_waypipe_owned_children_count; i++) {
		if (g_waypipe_owned_children[i].owner_key != owner_key) {
			continue;
		}
		if (pid_count < (int)(sizeof(pids) / sizeof(pids[0]))) {
			pids[pid_count++] = g_waypipe_owned_children[i].pid;
		}
	}
	pthread_mutex_unlock(&g_waypipe_owned_children_lock);

	for (int i = 0; i < pid_count; i++) {
		pid_t pid = pids[i];
		if (pid <= 0) {
			continue;
		}

		if (kill(pid, SIGTERM) == -1 && errno != ESRCH) {
			wp_debug("Owner shutdown SIGTERM failed for pid=%d owner=%" PRIu64 ": %s",
					(int)pid, owner_key, strerror(errno));
		}

		int status = 0;
		pid_t r = 0;
		for (int k = 0; k < 10; k++) {
			do {
				r = waitpid(pid, &status, WNOHANG);
			} while (r == -1 && errno == EINTR);

			if (r == pid || (r == -1 && errno == ECHILD)) {
				break;
			}

			struct timespec delay = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
			nanosleep(&delay, NULL);
		}

		if (r != pid && !(r == -1 && errno == ECHILD)) {
			if (kill(pid, SIGKILL) == -1 && errno != ESRCH) {
				wp_debug("Owner shutdown SIGKILL failed for pid=%d owner=%" PRIu64 ": %s",
						(int)pid, owner_key, strerror(errno));
			}

			do {
				r = waitpid(pid, &status, 0);
			} while (r == -1 && errno == EINTR);
		}

		waypipe_unregister_owned_child_pid(pid);
	}
}

/* ---- scoped child reaping (embedded mode) ---- */

bool embed_wait_for_scoped_child(pid_t *target_pid, int *status, int options)
{
	if (!target_pid || *target_pid <= 0) {
		return false;
	}

	int stat = 0;
	pid_t r;
	do {
		r = waitpid(*target_pid, &stat, options);
	} while (r == -1 && errno == EINTR);

	if (r == 0) {
		return false;
	}
	if (r == -1) {
		if (errno == ECHILD) {
			waypipe_unregister_owned_child_pid(*target_pid);
			*target_pid = 0;
			return false;
		}
		wp_error("waitpid(%d) failed: %s", (int)*target_pid,
				strerror(errno));
		return false;
	}

	if (status) {
		*status = stat;
	}
	waypipe_unregister_owned_child_pid(*target_pid);
	*target_pid = 0;
	return true;
}

/* ---- in-process SSH launcher ---- */

static waypipe_ssh_inprocess_main_func_t g_waypipe_ssh_inprocess_main_func =
		NULL;
static void *g_waypipe_ssh_inprocess_main_user_data = NULL;

void waypipe_set_ssh_inprocess_main_func(
		waypipe_ssh_inprocess_main_func_t p_func, void *p_user_data)
{
	g_waypipe_ssh_inprocess_main_func = p_func;
	g_waypipe_ssh_inprocess_main_user_data = p_user_data;
}

struct inprocess_ssh_thread_ctx {
	waypipe_ssh_inprocess_main_func_t func;
	void *user_data;
	char **argv;
	int argc;
	int status_fd;
	uint64_t owner_key;
};

static void free_inprocess_ssh_thread_argv(char **argv, int argc)
{
	if (!argv) {
		return;
	}
	for (int i = 0; i < argc; i++) {
		free(argv[i]);
	}
	free(argv);
}

static void free_inprocess_ssh_thread_ctx(struct inprocess_ssh_thread_ctx *ctx)
{
	if (!ctx) {
		return;
	}
	if (ctx->status_fd >= 0) {
		checked_close(ctx->status_fd);
		ctx->status_fd = -1;
	}
	free_inprocess_ssh_thread_argv(ctx->argv, ctx->argc);
	free(ctx);
}

static int duplicate_argv_for_inprocess_thread(char **src_argv, char ***dst_argv,
		int *dst_argc)
{
	if (!src_argv || !dst_argv || !dst_argc) {
		return -1;
	}

	int argc = 0;
	while (src_argv[argc] != NULL) {
		argc++;
	}

	char **copy = calloc((size_t)argc + 1, sizeof(char *));
	if (!copy) {
		return -1;
	}

	for (int i = 0; i < argc; i++) {
		copy[i] = strdup(src_argv[i]);
		if (!copy[i]) {
			free_inprocess_ssh_thread_argv(copy, argc);
			return -1;
		}
	}

	copy[argc] = NULL;
	*dst_argv = copy;
	*dst_argc = argc;
	return 0;
}

static void *run_inprocess_ssh_thread(void *data)
{
	struct inprocess_ssh_thread_ctx *ctx =
			(struct inprocess_ssh_thread_ctx *)data;
	if (!ctx) {
		return NULL;
	}

	int exit_code = ctx->func ?
			ctx->func(ctx->argc, ctx->argv, ctx->user_data) :
			EXIT_FAILURE;
	if (exit_code < 0 || exit_code > 255) {
		exit_code = EXIT_FAILURE;
	}

	if (ctx->status_fd >= 0) {
		(void)write(ctx->status_fd, &exit_code, sizeof(exit_code));
		checked_close(ctx->status_fd);
		ctx->status_fd = -1;
	}

	if (ctx->owner_key != 0) {
		waypipe_request_owner_shutdown(ctx->owner_key);
	}

	free_inprocess_ssh_thread_ctx(ctx);
	return NULL;
}

static int spawn_inprocess_ssh_thread(char **arglist, int *status_fd_out)
{
	if (!g_waypipe_ssh_inprocess_main_func || !arglist || !status_fd_out) {
		return -1;
	}

	*status_fd_out = -1;

	int pipe_fds[2] = {-1, -1};
	if (pipe(pipe_fds) == -1) {
		wp_error("Failed to create in-process ssh status pipe: %s",
				strerror(errno));
		return -1;
	}
	if (set_nonblocking(pipe_fds[0]) == -1 || set_cloexec(pipe_fds[0]) == -1 ||
			set_cloexec(pipe_fds[1]) == -1) {
		wp_error("Failed to configure in-process ssh status pipe");
		checked_close(pipe_fds[0]);
		checked_close(pipe_fds[1]);
		return -1;
	}

	struct inprocess_ssh_thread_ctx *ctx =
			calloc(1, sizeof(*ctx));
	if (!ctx) {
		wp_error("Failed to allocate in-process ssh thread context");
		checked_close(pipe_fds[0]);
		checked_close(pipe_fds[1]);
		return -1;
	}

	ctx->func = g_waypipe_ssh_inprocess_main_func;
	ctx->user_data = g_waypipe_ssh_inprocess_main_user_data;
	ctx->status_fd = pipe_fds[1];
	ctx->owner_key = waypipe_get_current_owner_key();
	if (duplicate_argv_for_inprocess_thread(arglist, &ctx->argv, &ctx->argc) !=
			0) {
		wp_error("Failed to duplicate in-process ssh argv for worker thread");
		checked_close(pipe_fds[0]);
		free_inprocess_ssh_thread_ctx(ctx);
		return -1;
	}

	pthread_attr_t attr;
	if (pthread_attr_init(&attr) != 0) {
		wp_error("Failed to initialize in-process ssh thread attributes");
		checked_close(pipe_fds[0]);
		free_inprocess_ssh_thread_ctx(ctx);
		return -1;
	}
	if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
		wp_error("Failed to configure detached in-process ssh thread");
		pthread_attr_destroy(&attr);
		checked_close(pipe_fds[0]);
		free_inprocess_ssh_thread_ctx(ctx);
		return -1;
	}

	pthread_t worker;
	if (pthread_create(&worker, &attr, run_inprocess_ssh_thread, ctx) != 0) {
		wp_error("Failed to create in-process ssh worker thread");
		pthread_attr_destroy(&attr);
		checked_close(pipe_fds[0]);
		free_inprocess_ssh_thread_ctx(ctx);
		return -1;
	}
	pthread_attr_destroy(&attr);

	*status_fd_out = pipe_fds[0];
	return 0;
}

int embed_spawn_or_fork(char **arglist, int *status_fd_out, pid_t *pid_out,
		int channelsock, int channel_folder_fd, int cwd_fd, bool vsock)
{
	if (!arglist || !status_fd_out || !pid_out) {
		return -1;
	}
	*status_fd_out = -1;
	*pid_out = 0;

	if (g_waypipe_ssh_inprocess_main_func) {
		if (waypipe_get_embedded_no_fork_mode()) {
			return spawn_inprocess_ssh_thread(arglist, status_fd_out);
		}

		pid_t inprocess_pid = fork();
		if (inprocess_pid == -1) {
			wp_error("Failed to fork in-process ssh runner: %s",
					strerror(errno));
			return -1;
		} else if (inprocess_pid == 0) {
			if (channelsock >= 0) {
				checked_close(channelsock);
			}
			if (!vsock && channel_folder_fd >= 0) {
				checked_close(channel_folder_fd);
			}
			if (cwd_fd >= 0) {
				checked_close(cwd_fd);
			}

			int ssh_argc = 0;
			while (arglist[ssh_argc] != NULL) {
				ssh_argc++;
			}

			int rc = g_waypipe_ssh_inprocess_main_func(
					ssh_argc, arglist,
					g_waypipe_ssh_inprocess_main_user_data);
			if (rc < 0 || rc > 255) {
				rc = EXIT_FAILURE;
			}
			_exit(rc);
		}
		*pid_out = inprocess_pid;
		return 0;
	}

	int err = posix_spawnp(pid_out, arglist[0], NULL, NULL, arglist, environ);
	if (err) {
		wp_error("Failed to spawn ssh process: %s", strerror(err));
		return -1;
	}
	return 0;
}

/* ---- in-process runner exit-code pipe helpers ---- */

static void quiet_close_fd(int *fd)
{
	if (!fd || *fd < 0) {
		return;
	}
	if (close(*fd) == -1 && errno != EBADF) {
		wp_debug("close(%d) failed in worker cleanup: %s", *fd,
				strerror(errno));
	}
	*fd = -1;
}

bool embed_try_read_exit_code(int *fd, int *exit_code)
{
	if (!fd || *fd < 0) {
		return false;
	}

	int code = EXIT_FAILURE;
	ssize_t nr = read(*fd, &code, sizeof(code));
	if (nr == (ssize_t)sizeof(code)) {
		if (exit_code) {
			*exit_code = code;
		}
		quiet_close_fd(fd);
		return true;
	}
	if (nr == 0) {
		if (exit_code) {
			*exit_code = EXIT_FAILURE;
		}
		quiet_close_fd(fd);
		return true;
	}
	if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
		return false;
	}

	if (nr < 0) {
		wp_error("Failed to read in-process ssh runner exit code: %s",
				strerror(errno));
	} else {
		wp_error("Short read while reading in-process ssh runner exit code: %zd bytes",
				nr);
	}

	if (exit_code) {
		*exit_code = EXIT_FAILURE;
	}
	quiet_close_fd(fd);
	return true;
}

bool embed_wait_for_exit_code(int *fd, int timeout_ms, int *exit_code)
{
	if (!fd || *fd < 0) {
		return false;
	}

	if (embed_try_read_exit_code(fd, exit_code)) {
		return true;
	}

	int remaining_ms = timeout_ms;
	while (*fd >= 0 && remaining_ms > 0) {
		struct pollfd pfd = {
			.fd = *fd,
			.events = POLLIN | POLLHUP | POLLERR,
			.revents = 0,
		};
		int wait_ms = remaining_ms < 50 ? remaining_ms : 50;
		int rc = poll(&pfd, 1, wait_ms);
		if (rc < 0) {
			if (errno == EINTR) {
				continue;
			}
			wp_error("poll on in-process ssh runner exit pipe failed: %s",
					strerror(errno));
			break;
		}
		remaining_ms -= wait_ms;
		if (embed_try_read_exit_code(fd, exit_code)) {
			return true;
		}
	}

	return embed_try_read_exit_code(fd, exit_code);
}

/* ---- embedded worker-thread path for new client connections ---- */

struct client_connection_thread_ctx {
	int cwd_fd;
	int chanclient;
	int linkfd;
	char *display_folder;
	struct sockaddr_un display_filename;
	struct main_config config;
};

static void *run_client_connection_thread(void *arg)
{
	struct client_connection_thread_ctx *ctx =
			(struct client_connection_thread_ctx *)arg;
	if (!ctx) {
		return NULL;
	}

	struct socket_path display_path = {
			.folder = ctx->display_folder,
			.filename = &ctx->display_filename,
	};

	int display_fd = -1;
	if (connect_to_socket(ctx->cwd_fd, display_path, NULL, &display_fd) ==
			-1) {
		quiet_close_fd(&ctx->chanclient);
		quiet_close_fd(&ctx->linkfd);
		free(ctx->display_folder);
		free(ctx);
		return NULL;
	}

	(void)main_interface_loop(ctx->chanclient, display_fd, ctx->linkfd,
			&ctx->config, true);

	quiet_close_fd(&ctx->chanclient);
	quiet_close_fd(&display_fd);
	quiet_close_fd(&ctx->linkfd);

	free(ctx->display_folder);
	free(ctx);
	return NULL;
}

static void apply_conn_header(uint32_t header, struct main_config *config)
{
	if (header & CONN_NO_DMABUF_SUPPORT) {
		if (config) {
			config->no_gpu = true;
		}
	}
	// todo: consider allowing to disable video encoding
}

bool embed_spawn_client_connection_worker(int cwd_fd,
		struct pollfd *other_fds, int n_other_fds, int chanclient,
		int linkfds[2], bool reconnectable, struct conn_map *connmap,
		const struct main_config *config,
		const struct socket_path disp_path,
		const struct connection_token *conn_id)
{
	if (!waypipe_get_embedded_no_fork_mode()) {
		return false;
	}

	struct client_connection_thread_ctx *ctx =
			calloc(1, sizeof(*ctx));
	if (!ctx) {
		wp_error("Failed to allocate connection worker thread context");
		goto fail_ps;
	}

	ctx->display_folder = strdup(disp_path.folder ? disp_path.folder : "");
	if (!ctx->display_folder) {
		wp_error("Failed to duplicate display folder path for worker thread");
		free(ctx);
		goto fail_ps;
	}

	ctx->cwd_fd = cwd_fd;
	/* Transfer ownership of the accepted fd directly to the worker.
	 * In embedded/no-fork mode this avoids any risk of closing the
	 * channel in the poll-loop teardown path before the worker starts. */
	ctx->chanclient = chanclient;
	ctx->linkfd = reconnectable ? linkfds[1] : -1;
	ctx->display_filename = *disp_path.filename;
	ctx->config = *config;
	apply_conn_header(conn_id->header, &ctx->config);

	pthread_attr_t attr;
	if (pthread_attr_init(&attr) != 0) {
		wp_error("Failed to initialize worker thread attributes");
		quiet_close_fd(&ctx->linkfd);
		free(ctx->display_folder);
		free(ctx);
		goto fail_ps;
	}
	if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) !=
				0) {
		wp_error("Failed to configure detached worker thread");
		pthread_attr_destroy(&attr);
		quiet_close_fd(&ctx->linkfd);
		free(ctx->display_folder);
		free(ctx);
		goto fail_ps;
	}

	pthread_t worker;
	if (pthread_create(&worker, &attr, run_client_connection_thread,
				ctx) != 0) {
		wp_error("Failed to create worker thread for new client connection");
		pthread_attr_destroy(&attr);
		quiet_close_fd(&ctx->linkfd);
		free(ctx->display_folder);
		free(ctx);
		goto fail_ps;
	}
	pthread_attr_destroy(&attr);

	if (reconnectable) {
		connmap->data[connmap->count++] =
				(struct conn_addr){.linkfd = linkfds[0],
						.token = *conn_id,
						.pid = 0};
	}

	/* drop_incoming_connection() will remove this entry from the
	 * poll arrays. Mark it as invalid now so the caller does not close
	 * the fd we just handed to the worker thread. */
	for (int i = 0; i < n_other_fds; i++) {
		if (other_fds[i].fd == chanclient) {
			other_fds[i].fd = -1;
			break;
		}
	}

	return true;

fail_ps:
	quiet_close_fd(&linkfds[0]);
	quiet_close_fd(&linkfds[1]);
	quiet_close_fd(&chanclient);
	return true;
}
