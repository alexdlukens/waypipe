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
#ifndef WAYPIPE_EMBED_H
#define WAYPIPE_EMBED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Forward declarations: the full types live in util.h / main.h, which embed.c
 * includes. Pointers and by-value parameters below only need the tags. */
struct conn_map;
struct connection_token;
struct main_config;
struct pollfd;
struct socket_path;

#ifdef __cplusplus
extern "C" {
#endif

/** Optional sink for fully formatted waypipe log lines. Embedders can set
 * this to mirror logs into their own buffers while preserving normal stderr
 * output. */
typedef void (*waypipe_log_sink_func_t)(const char *line, size_t len);
extern waypipe_log_sink_func_t waypipe_log_sink;

/** When true, waypipe is running in an embedded host process where forking
 * worker subprocesses is undesirable; worker threads are used instead. */
void waypipe_set_embedded_no_fork_mode(bool enabled);
bool waypipe_get_embedded_no_fork_mode(void);

/** Poll timeout helper. In embedded no-fork mode, negative (blocking)
 * timeouts are capped at 50 ms so loops can re-check shutdown state and
 * teardown in bounded time. */
int waypipe_poll_timeout_ms(int requested_timeout_ms);

/* Owner-key registry: every in-process session derives an owner key from its
 * running thread, and shutdown requests are scoped to that key so a failing
 * session only tears itself down. */
uint64_t waypipe_get_current_owner_key(void);
void waypipe_request_owner_shutdown(uint64_t owner_key);
void waypipe_clear_owner_shutdown(uint64_t owner_key);
bool waypipe_owner_shutdown_requested(uint64_t owner_key);
bool waypipe_owner_shutdown_requested_current_thread(void);

/* Owned-child registry: pids spawned on behalf of a session owner. In
 * embedded no-fork mode multiple sessions share one process, so child reaping
 * is scoped per pid; the registry is what lets a session terminate and reap
 * only its own children. */
void waypipe_register_owned_child_pid(uint64_t owner_key, pid_t pid);
void waypipe_unregister_owned_child_pid(pid_t pid);
void waypipe_force_terminate_owned_children(uint64_t owner_key);

/** Optional in-process SSH launcher hook for MODE_SSH.
 *
 * When configured, waypipe calls this function instead of invoking
 * `posix_spawnp("ssh", ...)`. In embedded no-fork mode, the callback runs on a
 * detached worker thread; otherwise it runs in a forked child process. The
 * callback receives argv in the same form as an ssh CLI call, starting with
 * argv[0] == "ssh".
 */
typedef int (*waypipe_ssh_inprocess_main_func_t)(
		int argc, char **argv, void *user_data);

void waypipe_set_ssh_inprocess_main_func(
		waypipe_ssh_inprocess_main_func_t p_func, void *p_user_data);

/** Scoped reaping for a single owned child. In embedded no-fork mode,
 * wait_for_pid_and_clean() must not reap other sessions' children, so it calls
 * this instead of the shared waitpid(-1) loop. Semantics match the scoped
 * branch of wait_for_pid_and_clean(): *target_pid is zeroed and unregistered
 * once the process has been reaped. */
bool embed_wait_for_scoped_child(pid_t *target_pid, int *status, int options);

/** Read the exit code written by an in-process ssh runner thread from the
 * status pipe. Returns true once the pipe is exhausted (exit code stored in
 * *exit_code when non-NULL), false if the read would block. The fd is closed
 * and set to -1 when exhausted. */
bool embed_try_read_exit_code(int *fd, int *exit_code);

/** Poll the in-process ssh runner status pipe until the exit code is
 * available or the timeout elapses. Returns true on success. */
bool embed_wait_for_exit_code(int *fd, int timeout_ms, int *exit_code);

/** Run the ssh command for MODE_SSH.
 *
 * If an in-process SSH hook is configured, this either spawns a detached
 * worker thread (embedded no-fork mode; the read end of the exit-code pipe is
 * returned in *status_fd_out) or forks a child that runs the hook and exits
 * with its return code (*pid_out). With no hook configured, posix_spawnp is
 * used (*pid_out). The forked child closes channelsock, channel_folder_fd and
 * cwd_fd before invoking the hook. Returns 0 on success, -1 on failure. */
int embed_spawn_or_fork(char **arglist, int *status_fd_out, pid_t *pid_out,
		int channelsock, int channel_folder_fd, int cwd_fd, bool vsock);

/** Handle a new client connection in embedded no-fork mode by spawning a
 * detached worker thread that runs the connection loop. Returns true when
 * embedded mode is in effect — the connection is owned by the worker thread
 * (or failure cleanup has already closed linkfds[] and chanclient) and the
 * caller must NOT fall through to the forking path. Returns false when waypipe
 * is not embedded, so the caller runs the normal fork() path. */
bool embed_spawn_client_connection_worker(int cwd_fd,
		struct pollfd *other_fds, int n_other_fds, int chanclient,
		int linkfds[2], bool reconnectable, struct conn_map *connmap,
		const struct main_config *config,
		const struct socket_path disp_path,
		const struct connection_token *conn_id);

#ifdef __cplusplus
}
#endif

#endif // WAYPIPE_EMBED_H
