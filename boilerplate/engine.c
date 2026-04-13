/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
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
#define _GNU_SOURCE
#include <sched.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "monitor_ioctl.h"

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP,
    CMD_STOP_ALL   // 👈 ADD THIS LINE
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
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
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
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

const char *state_to_string(container_state_t state)
{
    switch (state) {
        case CONTAINER_RUNNING: return "running";
        case CONTAINER_STOPPED: return "stopped";
        case CONTAINER_EXITED: return "exited";
        default: return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    (void)buffer;
    (void)item;
    return -1;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    (void)buffer;
    (void)item;
    return -1;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    (void)arg;
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
 int child_fn(void *arg)
 {
     control_request_t *req = (control_request_t *)arg;
 
     if (chroot(req->rootfs) != 0) {
         perror("chroot");
         return 1;
     }
 
     if (chdir("/") != 0) {
         perror("chdir");
         return 1;
     }
 
     if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
         perror("mount");
         return 1;
     }
 
     char *args[] = { req->command, NULL };
 
     execvp(args[0], args);
 
     perror("exec");
     return 1;
 }

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
 
 // 🔥 MAIN SUPERVISOR
 static int run_supervisor(const char *rootfs)
 {
     supervisor_ctx_t ctx;
     memset(&ctx, 0, sizeof(ctx));
 
     pthread_mutex_init(&ctx.metadata_lock, NULL);
 
     signal(SIGCHLD, sigchld_handler);
     int server_fd;
     struct sockaddr_un addr;
 
     // socket create
     server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
     if (server_fd < 0) {
         perror("socket");
         return 1;
     }
 
     unlink(CONTROL_PATH);
 
     memset(&addr, 0, sizeof(addr));
     addr.sun_family = AF_UNIX;
     strcpy(addr.sun_path, CONTROL_PATH);
 
     if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
         perror("bind");
         return 1;
     }
 
     if (listen(server_fd, 5) < 0) {
         perror("listen");
         return 1;
     }
 
     printf("Supervisor running...\n");
 
     while (1) {
         int client_fd = accept(server_fd, NULL, NULL);
         if (client_fd < 0) continue;
 
         control_request_t req;
         memset(&req, 0, sizeof(req));
 
         if (read(client_fd, &req, sizeof(req)) <= 0) {
             close(client_fd);
             continue;
         }
 
         // 🔹 START
         if (req.kind == CMD_START) {
             printf("Starting container: %s\n", req.container_id);
 
             char *stack = malloc(STACK_SIZE);
             if (!stack) {
                 perror("malloc");
                 close(client_fd);
                 continue;
             }
 
             pid_t pid = clone(
                 child_fn,
                 stack + STACK_SIZE,
                 CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD,
                 &req
             );
             int fd = open("/dev/container_monitor", O_RDWR);
                if (fd < 0) {
                    perror("open monitor");
                } else {
                    struct monitor_request mreq;
                    memset(&mreq, 0, sizeof(mreq));

                    mreq.pid = pid;
                    strncpy(mreq.container_id, req.container_id, sizeof(mreq.container_id));

                    mreq.soft_limit_bytes = req.soft_limit_bytes;
                    mreq.hard_limit_bytes = req.hard_limit_bytes;

                    if (ioctl(fd, MONITOR_REGISTER, &mreq) < 0) {
                        perror("ioctl REGISTER");
                    } else {
                        printf("Monitor registered for %s\n", req.container_id);
                    }

                    close(fd);
                }
                
             if (pid < 0) {
                 perror("clone");
                 write(client_fd, "FAIL", 4);
             } else {
                 printf("Container %s started (PID %d)\n",
                        req.container_id, pid);

                        int fd = open("/dev/container_monitor", O_RDWR);
                        if (fd >= 0) {
                            struct monitor_request mreq;
                            memset(&mreq, 0, sizeof(mreq));
                        
                            mreq.pid = pid;
                            strncpy(mreq.container_id, req.container_id, sizeof(mreq.container_id));
                        
                            // You can tune these
                            mreq.soft_limit_bytes = 50 * 1024 * 1024;   // 50MB
                            mreq.hard_limit_bytes = 100 * 1024 * 1024;  // 100MB
                        
                            if (ioctl(fd, MONITOR_REGISTER, &mreq) < 0) {
                                perror("ioctl REGISTER");
                            }
                        
                            close(fd);
                        } else {
                            perror("open /dev/container_monitor");
                        }
 
                 // add to list
                 container_record_t *c = malloc(sizeof(*c));
                 memset(c, 0, sizeof(*c));
 
                 strcpy(c->id, req.container_id);
                 c->host_pid = pid;
                 c->state = CONTAINER_RUNNING;
                 c->soft_limit_bytes = req.soft_limit_bytes;
                 c->hard_limit_bytes = req.hard_limit_bytes;
 
                 pthread_mutex_lock(&ctx.metadata_lock);
                 c->next = ctx.containers;
                 ctx.containers = c;
                 pthread_mutex_unlock(&ctx.metadata_lock);
 
                 write(client_fd, "OK", 2);
             }
         }
 
         // 🔹 STOP ONE
         else if (req.kind == CMD_STOP) {
             pthread_mutex_lock(&ctx.metadata_lock);
 
             container_record_t *cur = ctx.containers;
 
             while (cur) {
                 if (strcmp(cur->id, req.container_id) == 0) {
                     kill(cur->host_pid, SIGKILL);
                     cur->state = CONTAINER_EXITED;
                     write(client_fd, "STOPPED", 7);
                     break;
                 }
                 cur = cur->next;
             }
 
             pthread_mutex_unlock(&ctx.metadata_lock);
         }
 
         // 🔹 STOP ALL
         else if (req.kind == CMD_STOP_ALL) {
             pthread_mutex_lock(&ctx.metadata_lock);
 
             container_record_t *cur = ctx.containers;
 
             while (cur) {
                 kill(cur->host_pid, SIGKILL);
                 cur->state = CONTAINER_EXITED;
                 cur = cur->next;
             }
 
             pthread_mutex_unlock(&ctx.metadata_lock);
 
             write(client_fd, "ALL STOPPED\n", 12);
         }
 
         // 🔹 PS
         else if (req.kind == CMD_PS) {
             pthread_mutex_lock(&ctx.metadata_lock);
 
             container_record_t *cur = ctx.containers;
 
             char buffer[512];
             int offset = 0;
 
             offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                "ID\tPID\tSTATE\n");
 
             while (cur) {
                 offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                    "%s\t%d\t%s\n",
                                    cur->id,
                                    cur->host_pid,
                                    cur->state == CONTAINER_RUNNING ? "running" : "exited");
 
                 cur = cur->next;
             }
 
             pthread_mutex_unlock(&ctx.metadata_lock);
 
             write(client_fd, buffer, strlen(buffer));
         }
 
         close(client_fd);
 
         // 🔥 RESOURCE MONITORINg
         sleep(1);  // VERY IMPORTANT
     }
 
     return 0;
 }

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */

 int send_control_request(control_request_t *req)
 {
     int fd;
     struct sockaddr_un addr;
 
     // create socket
     fd = socket(AF_UNIX, SOCK_STREAM, 0);
     if (fd < 0) {
         perror("socket");
         return 1;
     }
 
     memset(&addr, 0, sizeof(addr));
     addr.sun_family = AF_UNIX;
     strcpy(addr.sun_path, CONTROL_PATH);
 
     // connect to supervisor
     if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
         perror("connect");
         close(fd);
         return 1;
     }
 
     // send request
     if (write(fd, req, sizeof(*req)) <= 0) {
         perror("write");
         close(fd);
         return 1;
     }
 
     // read response
     char buffer[1024];
     int n = read(fd, buffer, sizeof(buffer) - 1);
     if (n > 0) {
         buffer[n] = '\0';
         printf("%s", buffer);
     } else {
         perror("read");
     }
 
     close(fd);
     return 0;
 }

 static int cmd_start(int argc, char *argv[])
 {
     control_request_t req;
 
     if (argc < 5) {
         fprintf(stderr,
                 "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                 argv[0]);
         return 1;
     }
 
     memset(&req, 0, sizeof(req));
     req.kind = CMD_START;
 
     strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
     strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
     strncpy(req.command, argv[4], sizeof(req.command) - 1);
 
     req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
     req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
 
     if (parse_optional_flags(&req, argc, argv, 5) != 0)
         return 1;
 
     return send_control_request(&req);
 }


static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0) {
        return 1;
    }
    
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

static int cmd_stop_all(int argc, char *argv[])
{
    control_request_t req;
    memset(&req, 0, sizeof(req));

    req.kind = CMD_STOP_ALL;

    return send_control_request(&req);
}


int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    else if (strcmp(argv[1], "stop-all") == 0)
        return cmd_stop_all(argc, argv);

    usage(argv[0]);
    return 1;
}
