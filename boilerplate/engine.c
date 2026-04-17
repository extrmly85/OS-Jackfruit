/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Student: Sudhanwa
 * SRN: PES2UG24CS531
 *
 * Implements Tasks 1, 2, 3, and 6:
 *   Task 1 - Multi-container supervisor with clone/namespaces
 *   Task 2 - CLI and control-plane IPC via UNIX domain socket
 *   Task 3 - Bounded-buffer logging (producer-consumer, pipes)
 *   Task 6 - Clean resource teardown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define STACK_SIZE           (1024 * 1024)
#define CONTAINER_ID_LEN     32
#define CONTROL_PATH         "/tmp/mini_runtime.sock"
#define LOG_DIR              "logs"
#define CONTROL_MESSAGE_LEN  4096   /* enlarged for ps output */
#define CHILD_COMMAND_LEN    256
#define LOG_CHUNK_SIZE       4096
#define LOG_BUFFER_CAPACITY  16
#define DEFAULT_SOFT_LIMIT   (40UL << 20)
#define DEFAULT_HARD_LIMIT   (64UL << 20)
#define MAX_LOG_PATH         (PATH_MAX)

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ------------------------------------------------------------------ */
/* Data Structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct container_record {
    char               id[CONTAINER_ID_LEN];
    pid_t              host_pid;
    time_t             started_at;
    container_state_t  state;
    unsigned long      soft_limit_bytes;
    unsigned long      hard_limit_bytes;
    int                exit_code;
    int                exit_signal;
    int                stop_requested;    /* set before SIGTERM from 'stop' */
    char               log_path[MAX_LOG_PATH];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  pipe_write_fd;   /* supervisor end: child writes stdout/err here */
} child_config_t;

/* per-producer thread args */
typedef struct {
    int              read_fd;          /* pipe read end in supervisor */
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} producer_args_t;

typedef struct {
    int                  server_fd;
    int                  monitor_fd;
    volatile sig_atomic_t should_stop;
    pthread_t            logger_thread;
    bounded_buffer_t     log_buffer;
    pthread_mutex_t      metadata_lock;
    container_record_t  *containers;
} supervisor_ctx_t;

/* global pointer so signal handlers can reach the context */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Usage and argument parsing                                          */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long  nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1],
                               &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1],
                               &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded buffer (Task 3)                                             */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc) return rc;
    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc) { pthread_mutex_destroy(&buf->mutex); return rc; }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/*
 * bounded_buffer_push - producer inserts one item.
 * Blocks while buffer is full (unless shutting down).
 * Returns 0 on success, -1 if shutdown in progress.
 */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    /* Wait while full */
    while (buf->count >= LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * bounded_buffer_pop - consumer removes one item.
 * Blocks while empty (unless shutting down).
 * Returns 0 on success, -1 when shutdown and buffer drained.
 */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == 0) {
        if (buf->shutting_down) {
            pthread_mutex_unlock(&buf->mutex);
            return -1;
        }
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Logging consumer thread (Task 3)                                   */
/* ------------------------------------------------------------------ */

/*
 * logging_thread - single consumer.
 * Reads from the bounded buffer, opens per-container log files,
 * and writes each chunk to the right file.
 * Exits once shutdown is signalled and buffer is empty.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char   log_path[MAX_LOG_PATH];
        FILE  *f;

        /* Find the log path for this container */
        log_path[0] = '\0';
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strcmp(c->id, item.container_id) == 0) {
                strncpy(log_path, c->log_path, sizeof(log_path) - 1);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0')
            continue;   /* container gone; discard */

        f = fopen(log_path, "a");
        if (!f)
            continue;
        fwrite(item.data, 1, item.length, f);
        fclose(f);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Producer thread: reads one container's pipe into the buffer        */
/* ------------------------------------------------------------------ */

static void *producer_thread(void *arg)
{
    producer_args_t  *pa  = (producer_args_t *)arg;
    bounded_buffer_t *buf = pa->log_buffer;
    log_item_t        item;
    ssize_t           n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, pa->container_id,
            sizeof(item.container_id) - 1);

    while ((n = read(pa->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(buf, &item);
        memset(item.data, 0, (size_t)n);
    }

    close(pa->read_fd);
    free(pa);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* kernel-module helpers (already present in boilerplate; kept here)  */
/* ------------------------------------------------------------------ */

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    if (monitor_fd < 0) return 0;
    memset(&req, 0, sizeof(req));
    req.pid              = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                            const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    if (monitor_fd < 0) return 0;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Container child entrypoint (Task 1)                                */
/* ------------------------------------------------------------------ */

/*
 * child_fn - runs inside the new namespace after clone().
 * 1. Redirect stdout/stderr to the supervisor pipe.
 * 2. chroot into the container's rootfs.
 * 3. Mount /proc.
 * 4. Apply nice value.
 * 5. execve the user command.
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr into the logging pipe */
    if (dup2(cfg->pipe_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->pipe_write_fd, STDERR_FILENO) < 0) {
        _exit(1);
    }
    close(cfg->pipe_write_fd);

    /* chroot into the per-container rootfs */
    if (chroot(cfg->rootfs) < 0) {
        fprintf(stderr, "chroot(%s) failed: %s\n",
                cfg->rootfs, strerror(errno));
        _exit(1);
    }
    if (chdir("/") < 0) {
        fprintf(stderr, "chdir / failed: %s\n", strerror(errno));
        _exit(1);
    }

    /* Mount /proc so 'ps' and /proc/self work inside */
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        fprintf(stderr, "mount /proc failed: %s\n", strerror(errno));
        /* non-fatal; container may still function */
    }

    /* Apply scheduler priority via nice */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) < 0) {
            fprintf(stderr, "nice(%d) failed: %s\n",
                    cfg->nice_value, strerror(errno));
            /* non-fatal */
        }
    }

    /* Execute the requested command through /bin/sh */
    char *sh_argv[] = { "/bin/sh", "-c", cfg->command, NULL };
    execve("/bin/sh", sh_argv, NULL);

    /* If execve returns, something went wrong */
    fprintf(stderr, "execve failed: %s\n", strerror(errno));
    _exit(1);
}

/* ------------------------------------------------------------------ */
/* Metadata helpers                                                   */
/* ------------------------------------------------------------------ */

static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Signal handlers (Task 2 / Task 6)                                  */
/* ------------------------------------------------------------------ */

static void handle_sigterm(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

/*
 * handle_sigchld - reap all exited children and update metadata.
 * Uses WNOHANG so it never blocks.
 */
static void handle_sigchld(int sig)
{
    (void)sig;
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx)
            continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code = WEXITSTATUS(status);
                    c->state     = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    /*
                     * Task 4 attribution rule:
                     *  - SIGKILL without stop_requested → hard-limit kill
                     *  - stop_requested → STOPPED
                     */
                    if (c->stop_requested)
                        c->state = CONTAINER_STOPPED;
                    else
                        c->state = CONTAINER_KILLED;
                }
                /* Unregister from kernel monitor */
                unregister_from_monitor(g_ctx->monitor_fd,
                                        c->id, c->host_pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

/* ------------------------------------------------------------------ */
/* Supervisor command handlers                                        */
/* ------------------------------------------------------------------ */

/*
 * handle_start - spawn one container child via clone().
 * Creates a pipe for logging, allocates stack, clones with
 * PID + UTS + mount namespace flags, registers with the kernel
 * module, and starts a producer thread to read the pipe.
 */
static int handle_start(supervisor_ctx_t *ctx,
                        const control_request_t *req,
                        char *resp_msg,
                        size_t resp_len)
{
    int             pipefd[2];
    char           *stack = NULL;
    pid_t           child_pid;
    child_config_t  cfg;
    container_record_t *rec;
    producer_args_t    *pa;
    pthread_t           ptid;
    char                log_path[MAX_LOG_PATH];

    /* Check for duplicate container ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(resp_msg, resp_len,
                 "ERROR: container '%s' already exists", req->container_id);
        return 1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create log directory and file path */
    mkdir(LOG_DIR, 0755);
    snprintf(log_path, sizeof(log_path),
             LOG_DIR "/%s.log", req->container_id);

    /* Create the logging pipe */
    if (pipe(pipefd) < 0) {
        snprintf(resp_msg, resp_len, "ERROR: pipe: %s", strerror(errno));
        return 1;
    }
    /* Make read end non-blocking so producer can drain on exit */
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    /* Build child config */
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.id,      req->container_id, sizeof(cfg.id) - 1);
    strncpy(cfg.rootfs,  req->rootfs,       sizeof(cfg.rootfs) - 1);
    strncpy(cfg.command, req->command,      sizeof(cfg.command) - 1);
    cfg.nice_value   = req->nice_value;
    cfg.pipe_write_fd = pipefd[1];

    /* Allocate stack for clone */
    stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipefd[0]); close(pipefd[1]);
        snprintf(resp_msg, resp_len, "ERROR: malloc stack failed");
        return 1;
    }

    /* Clone with PID, UTS, and mount namespaces */
    child_pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      &cfg);
    free(stack);
    /* Close the write end in the supervisor – child owns it */
    close(pipefd[1]);

    if (child_pid < 0) {
        close(pipefd[0]);
        snprintf(resp_msg, resp_len, "ERROR: clone: %s", strerror(errno));
        return 1;
    }

    /* Allocate container metadata */
    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        kill(child_pid, SIGKILL);
        close(pipefd[0]);
        snprintf(resp_msg, resp_len, "ERROR: calloc metadata");
        return 1;
    }
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->host_pid         = child_pid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->stop_requested   = 0;
    strncpy(rec->log_path, log_path, sizeof(rec->log_path) - 1);

    /* Insert into list */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel memory monitor */
    register_with_monitor(ctx->monitor_fd,
                          req->container_id, child_pid,
                          req->soft_limit_bytes, req->hard_limit_bytes);

    /* Start a producer thread to drain the pipe into the log buffer */
    pa = malloc(sizeof(*pa));
    if (pa) {
        pa->read_fd   = pipefd[0];
        pa->log_buffer = &ctx->log_buffer;
        strncpy(pa->container_id, req->container_id,
                sizeof(pa->container_id) - 1);
        pthread_create(&ptid, NULL, producer_thread, pa);
        pthread_detach(ptid);
    } else {
        close(pipefd[0]);
    }

    snprintf(resp_msg, resp_len,
             "OK: started container '%s' pid=%d",
             req->container_id, child_pid);
    return 0;
}

/*
 * handle_run - like handle_start but supervisor waits for exit
 * and returns the container's exit code in the response.
 */
static int handle_run(supervisor_ctx_t *ctx,
                      const control_request_t *req,
                      char *resp_msg,
                      size_t resp_len)
{
    int rc = handle_start(ctx, req, resp_msg, resp_len);
    if (rc != 0)
        return rc;

    /* Find the PID we just created */
    pid_t target = -1;
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (c) target = c->host_pid;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (target <= 0) {
        snprintf(resp_msg, resp_len, "ERROR: run could not find container");
        return 1;
    }

    /* Wait for the container to finish (SIGCHLD handler reaps it) */
    int exited = 0;
    while (!exited) {
        usleep(100 * 1000); /* 100 ms poll */
        pthread_mutex_lock(&ctx->metadata_lock);
        c = find_container(ctx, req->container_id);
        if (c && (c->state == CONTAINER_EXITED ||
                  c->state == CONTAINER_STOPPED ||
                  c->state == CONTAINER_KILLED))
            exited = 1;
        pthread_mutex_unlock(&ctx->metadata_lock);
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    c = find_container(ctx, req->container_id);
    int code = c ? c->exit_code : -1;
    const char *st = c ? state_to_string(c->state) : "unknown";
    snprintf(resp_msg, resp_len,
             "OK: container '%s' finished state=%s exit_code=%d",
             req->container_id, st, code);
    pthread_mutex_unlock(&ctx->metadata_lock);
    return code;
}

/*
 * handle_ps - build a table of all containers for the CLI.
 */
static int handle_ps(supervisor_ctx_t *ctx,
                     char *resp_msg,
                     size_t resp_len)
{
    int off = 0;

    off += snprintf(resp_msg + off, resp_len - (size_t)off,
                    "%-16s %-8s %-10s %-10s %-10s\n",
                    "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)");
    off += snprintf(resp_msg + off, resp_len - (size_t)off,
                    "%-16s %-8s %-10s %-10s %-10s\n",
                    "----------------", "--------",
                    "----------", "----------", "----------");

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    while (c) {
        off += snprintf(resp_msg + off, resp_len - (size_t)off,
                        "%-16s %-8d %-10s %-10lu %-10lu\n",
                        c->id, c->host_pid,
                        state_to_string(c->state),
                        c->soft_limit_bytes >> 20,
                        c->hard_limit_bytes >> 20);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    return 0;
}

/*
 * handle_logs - read the log file for a container and send it back.
 */
static int handle_logs(supervisor_ctx_t *ctx,
                       const control_request_t *req,
                       char *resp_msg,
                       size_t resp_len)
{
    char log_path[MAX_LOG_PATH];
    FILE *f;
    size_t n;

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(resp_msg, resp_len,
                 "ERROR: no container '%s'", req->container_id);
        return 1;
    }
    strncpy(log_path, c->log_path, sizeof(log_path) - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    f = fopen(log_path, "r");
    if (!f) {
        /* Use a fixed-size format that won't truncate */
        snprintf(resp_msg, resp_len, "ERROR: cannot open log: %s",
                 strerror(errno));
        return 1;
    }
    n = fread(resp_msg, 1, resp_len - 1, f);
    resp_msg[n] = '\0';
    fclose(f);
    return 0;
}

/*
 * handle_stop - signal a container to stop.
 * Sets stop_requested so the SIGCHLD handler classifies it correctly.
 */
static int handle_stop(supervisor_ctx_t *ctx,
                       const control_request_t *req,
                       char *resp_msg,
                       size_t resp_len)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(resp_msg, resp_len,
                 "ERROR: no container '%s'", req->container_id);
        return 1;
    }
    if (c->state != CONTAINER_RUNNING && c->state != CONTAINER_STARTING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(resp_msg, resp_len,
                 "ERROR: container '%s' is not running", req->container_id);
        return 1;
    }
    c->stop_requested = 1;
    pid_t pid = c->host_pid;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Send SIGTERM first; SIGCHLD handler will reap */
    if (kill(pid, SIGTERM) < 0) {
        snprintf(resp_msg, resp_len,
                 "ERROR: kill SIGTERM pid=%d: %s", pid, strerror(errno));
        return 1;
    }
    snprintf(resp_msg, resp_len,
             "OK: sent SIGTERM to container '%s' pid=%d",
             req->container_id, pid);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main supervisor event loop (Task 1 + 2)                            */
/* ------------------------------------------------------------------ */

static int dispatch_request(supervisor_ctx_t  *ctx,
                             const control_request_t *req,
                             control_response_t      *resp)
{
    memset(resp, 0, sizeof(*resp));
    switch (req->kind) {
    case CMD_START:
        resp->status = handle_start(ctx, req,
                                    resp->message,
                                    sizeof(resp->message));
        break;
    case CMD_RUN:
        resp->status = handle_run(ctx, req,
                                  resp->message,
                                  sizeof(resp->message));
        break;
    case CMD_PS:
        resp->status = handle_ps(ctx, resp->message, sizeof(resp->message));
        break;
    case CMD_LOGS:
        resp->status = handle_logs(ctx, req,
                                   resp->message,
                                   sizeof(resp->message));
        break;
    case CMD_STOP:
        resp->status = handle_stop(ctx, req,
                                   resp->message,
                                   sizeof(resp->message));
        break;
    default:
        snprintf(resp->message, sizeof(resp->message), "ERROR: unknown cmd");
        resp->status = 1;
        break;
    }
    return resp->status;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t    ctx;
    struct sockaddr_un  addr;
    struct sigaction    sa;
    int                 rc;

    (void)rootfs; /* rootfs path provided for context; not used directly */

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* --- initialise locks and buffers --- */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc) { errno = rc; perror("bounded_buffer_init");
              pthread_mutex_destroy(&ctx.metadata_lock); return 1; }

    /* --- open kernel monitor device (non-fatal if absent) --- */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] /dev/container_monitor not available"
                        " (memory limits disabled)\n");

    /* --- create the UNIX domain socket for CLI ↔ supervisor IPC --- */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); goto cleanup; }

    unlink(CONTROL_PATH); /* remove stale socket */

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); goto cleanup;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen"); goto cleanup;
    }

    /* --- install signal handlers --- */
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = SA_RESTART;
    sa.sa_handler = handle_sigchld;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = handle_sigterm;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* --- start logger (consumer) thread --- */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc) { errno = rc; perror("pthread_create logger"); goto cleanup; }

    /* --- create log directory --- */
    mkdir(LOG_DIR, 0755);

    fprintf(stderr, "[supervisor] started. socket=%s\n", CONTROL_PATH);

    /* ============================================================
     * Main event loop: accept CLI connections, dispatch requests.
     * ============================================================ */
    while (!ctx.should_stop) {
        int client_fd;
        control_request_t  req;
        control_response_t resp;
        ssize_t            n;

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue; /* interrupted by signal */
            if (!ctx.should_stop) perror("accept");
            break;
        }

        /* Read the full request struct */
        n = read(client_fd, &req, sizeof(req));
        if (n == (ssize_t)sizeof(req)) {
            dispatch_request(&ctx, &req, &resp);
            /* Send full response; ignore partial-write on closed client */
            if (write(client_fd, &resp, sizeof(resp)) < 0)
                perror("write response");
        }
        close(client_fd);
    }

    fprintf(stderr, "[supervisor] shutting down...\n");

    /* --- orderly shutdown (Task 6) --- */

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING ||
            c->state == CONTAINER_STARTING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Give containers 2 s to exit, then SIGKILL */
    sleep(2);
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING ||
            c->state == CONTAINER_STARTING)
            kill(c->host_pid, SIGKILL);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Drain remaining log buffer items */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

cleanup:
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.server_fd >= 0) { close(ctx.server_fd); unlink(CONTROL_PATH); }
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    g_ctx = NULL;
    fprintf(stderr, "[supervisor] exited cleanly.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI client side (Task 2)                                           */
/* ------------------------------------------------------------------ */

/*
 * send_control_request - connect to the running supervisor,
 * send a request struct, read the response and print it.
 */
static int send_control_request(const control_request_t *req)
{
    int                 sock;
    struct sockaddr_un  addr;
    control_response_t  resp;
    ssize_t             n;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "Cannot connect to supervisor at %s: %s\n"
                "  Is the supervisor running?\n"
                "  Try: sudo ./engine supervisor <rootfs-path>\n",
                CONTROL_PATH, strerror(errno));
        close(sock);
        return 1;
    }

    n = write(sock, req, sizeof(*req));
    if (n != (ssize_t)sizeof(*req)) {
        perror("write request"); close(sock); return 1;
    }

    n = read(sock, &resp, sizeof(resp));
    close(sock);
    if (n != (ssize_t)sizeof(resp)) {
        perror("read response"); return 1;
    }

    /* Print the supervisor's message */
    if (resp.message[0] != '\0')
        printf("%s\n", resp.message);

    return resp.status;
}

/* ------------------------------------------------------------------ */
/* CLI command builders                                               */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind              = CMD_START;
    req.soft_limit_bytes  = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes  = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind             = CMD_RUN;
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
