/**
 * @brief   Master process implementing a pre-fork worker pool, where each
 *          worker runs its own io_uring event loop
 *
 * Architecture overview
 * ---------------------
 * The master:
 *   - creates and binds the listening socket
 *   - initializes POSIX shared memory for live metrics
 *   - forks NUM_WORKERS workers that all accept() on the same socket
 *   - supervises the workers and respawns any that die
 *   - on SIGINT/SIGTERM, stops the workers and unlinks the IPC resources
 *
 * Each worker drives accept -> read -> (parse + build response) -> write
 * -> close through an io_uring submission/completion queue
 *
 * Connection lifecycle (per worker)
 * ---------------------------------
 *   EVENT_ACCEPT --> EVENT_READ --> EVENT_WRITE --> close
 *
 * Every in-flight operation owns a heap-allocated conn_context_t that is
 * attached to its SQE as user data and recovered from the matching CQE.
 * Exactly one party owns a context at any time; whoever finishes the
 * connection (or the accept) is responsible for releasing it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <liburing.h>

#include "uring_server.h"
#include "http_handler.h"
#include "ipc_manager.h"
#include "server_stats.h"


#define DEFAULT_PORT 8080   /* Port used when none is given on the command line */
#define BACKLOG      512    /* Pending-connection queue length passed to listen() */
#define NUM_WORKERS  4      /* Number of pre-forked worker processes */


/* PIDs of the live workers, indexed by worker id. A value <= 0 marks a
 * free slot that the supervisor loop will (re)populate. Only meaningful
 * in the master process */
static pid_t worker_pids[NUM_WORKERS];

/* Cleared by the signal handlers to request shutdown. Each process has its
 * own copy after fork(), so the master and workers stop independently
 * sig_atomic_t guarantees safe access from within a signal handler */
static volatile sig_atomic_t server_running = 1;

/* Signal handlers                                                     */
/**
 * @brief Master signal handler (SIGINT/SIGTERM)
 *
 * Only sets a flag; the supervisor loop in main() performs the actual
 * shutdown work, keeping the handler async-signal-safe
 */
static void handle_master_signal(int sig) {
    (void)sig;
    server_running = 0;
}

/**
 * @brief Worker signal handler (SIGINT/SIGTERM)
 *
 * Sets the flag so the io_uring loop exits; the signal also interrupts
 * io_uring_wait_cqe(), which then returns -EINTR and lets the loop
 * re-evaluate the flag
 */
static void handle_worker_signal(int sig) {
    (void)sig;
    server_running = 0;
}

/**
 * @brief Install a signal handler via sigaction()
 *
 * @param signo   Signal number to handle
 * @param handler Handler function to invoke
 */
static void install_handler(int signo, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: we want blocking calls to be interrupted */
    sigaction(signo, &sa, NULL);
}

/* io_uring helpers                                                    */
/**
 * @brief Obtain a submission queue entry, flushing the queue once if full
 *
 * @param ring Initialized io_uring instance
 * @return A free SQE, or NULL if none is available even after flushing
 */
static struct io_uring_sqe *get_sqe(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        /* Submission queue is full: hand pending entries to the kernel
         * to free up slots, then retry once */
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    return sqe;
}

/**
 * @brief Queue an asynchronous accept on the listening socket
 *
 * Allocates a context tagged EVENT_ACCEPT. On success the context is owned
 * by the in-flight operation and released when its completion is handled
 *
 * @param ring        Initialized io_uring instance
 * @param server_sock Listening socket descriptor
 * @return 0 on success, -1 on allocation or queue failure
 */
static int queue_accept(struct io_uring *ring, int server_sock) {
    conn_context_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return -1;

    struct io_uring_sqe *sqe = get_sqe(ring);
    if (!sqe) {
        free(ctx); /* No SQE: nothing will complete, so release the context now */
        return -1;
    }

    ctx->fd = server_sock;
    ctx->event_type = EVENT_ACCEPT;
    ctx->bytes_transferred = 0;
    ctx->total_to_write = 0;

    /* NULL address/length: the peer address is not needed */
    io_uring_prep_accept(sqe, server_sock, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, ctx);
    return 0;
}

/**
 * @brief Queue an asynchronous read (recv) on a client socket
 *
 * Allocates a fresh context for the connection; this context is reused for
 * the subsequent write and freed when the connection is closed
 *
 * @param ring      Initialized io_uring instance
 * @param client_fd Connected client socket descriptor
 * @return 0 on success, -1 on allocation or queue failure
 */
static int queue_read(struct io_uring *ring, int client_fd) {
    conn_context_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return -1;

    struct io_uring_sqe *sqe = get_sqe(ring);
    if (!sqe) {
        free(ctx);
        return -1;
    }

    ctx->fd = client_fd;
    ctx->event_type = EVENT_READ;
    ctx->bytes_transferred = 0;
    ctx->total_to_write = 0;

    /* Reserve one byte so the received data can be NUL-terminated later */
    io_uring_prep_recv(sqe, client_fd, ctx->buffer, BUFFER_SIZE - 1, 0);
    io_uring_sqe_set_data(sqe, ctx);
    return 0;
}

/**
 * @brief Queue an asynchronous write (send) of ctx->buffer[0..total_to_write)
 *
 * Reuses the existing connection context, switching its state to EVENT_WRITE
 * On failure the caller retains ownership and must close the connection
 *
 * @param ring Initialized io_uring instance
 * @param ctx  Connection context holding the response to send
 * @return 0 on success, -1 if no SQE is available
 */
static int queue_write(struct io_uring *ring, conn_context_t *ctx) {
    struct io_uring_sqe *sqe = get_sqe(ring);
    if (!sqe) return -1;

    ctx->event_type = EVENT_WRITE;
    /* MSG_NOSIGNAL prevents SIGPIPE if the peer has already disconnected */
    io_uring_prep_send(sqe, ctx->fd, ctx->buffer, ctx->total_to_write, MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, ctx);
    return 0;
}

/**
 * @brief Close the client socket and release its context
 *
 * @param ctx Connection context; must not be used after this call
 */
static void close_connection(conn_context_t *ctx) {
    close(ctx->fd);
    free(ctx);
}

/* Worker                                                              */
/**
 * @brief Worker event loop powered by io_uring. Never returns.
 *
 * Sets up a private ring (preferring SQPOLL), primes it with an accept,
 * then processes completions until a shutdown signal arrives. Each
 * completion advances the connection state machine:
 *   ACCEPT -> start READ and re-arm ACCEPT
 *   READ   -> parse request, build response, start WRITE
 *   WRITE  -> close connection (no keep-alive)
 *
 * @param server_sock Listening socket inherited from the master
 * @param worker_id   Logical worker index (used for logging)
 * @param stats       Shared-memory metrics block updated by the HTTP handler
 */
static void __attribute__((noreturn))
run_worker_loop(int server_sock, int worker_id, server_metrics_t *stats) {
    struct io_uring ring;

    /* Stop cleanly on SIGTERM (from master) or SIGINT (Ctrl+C hits the whole group) */
    install_handler(SIGTERM, handle_worker_signal);
    install_handler(SIGINT, handle_worker_signal);

    /* Try kernel submission polling (SQPOLL) first; fall back to a normal ring */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000; /* ms before the kernel poll thread sleeps when idle */

    int use_sqpoll = 1;
    int ret = io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params);
    if (ret < 0) {
        /* SQPOLL may be unsupported or restricted (kernel version, privileges,
         * container policy). Degrade gracefully to a standard ring */
        fprintf(stderr, "[Worker %d] SQPOLL unavailable (%s), falling back to standard io_uring\n",
                worker_id, strerror(-ret));
        use_sqpoll = 0;
        ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
        if (ret < 0) {
            fprintf(stderr, "[Worker %d] io_uring_queue_init failed: %s\n",
                    worker_id, strerror(-ret));
            exit(EXIT_FAILURE);
        }
    }

    printf("[Worker %d (PID %d)] io_uring ring initialized (%s)\n",
           worker_id, getpid(), use_sqpoll ? "SQPOLL" : "standard");

    /* Prime the loop with the first accept */
    if (queue_accept(&ring, server_sock) < 0) {
        fprintf(stderr, "[Worker %d] failed to queue initial accept\n", worker_id);
        io_uring_queue_exit(&ring);
        exit(EXIT_FAILURE);
    }
    io_uring_submit(&ring);

    /* ---- Main completion loop ---- */
    while (server_running) {
        struct io_uring_cqe *cqe;

        /* Block until at least one completion is available */
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue; /* interrupted by a signal; re-check flag */
            fprintf(stderr, "[Worker %d] io_uring_wait_cqe: %s\n", worker_id, strerror(-ret));
            break;
        }

        /* Extract the result and context, then mark the CQE consumed so
         * its slot can be reused by the kernel */
        conn_context_t *ctx = io_uring_cqe_get_data(cqe);
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);

        if (res < 0) {
            /* I/O error on this operation (res holds -errno) */
            if (ctx->event_type == EVENT_ACCEPT) {
                free(ctx);
                /* Keep the listener alive unless we are shutting down */
                if (server_running && res != -ECANCELED) {
                    queue_accept(&ring, server_sock);
                }
            } else {
                /* Read/write failed: drop the connection */
                close_connection(ctx);
            }
        } else {
            switch (ctx->event_type) {
            case EVENT_ACCEPT: {
                /* On success, res is the new client file descriptor */
                int client_fd = res;

                if (queue_read(&ring, client_fd) < 0) {
                    close(client_fd);
                }
                /* Re-arm the listener so we keep accepting new connections */
                queue_accept(&ring, server_sock);
                free(ctx); /* The accept context is single-use */
                break;
            }

            case EVENT_READ: {
                /* On success, res is the number of bytes received */
                if (res == 0) {
                    /* Client closed the connection (EOF) */
                    close_connection(ctx);
                    break;
                }

                /* NUL-terminate so the handler can treat the request as a string */
                ctx->buffer[res] = '\0';

                /* Build the response in a separate buffer so request and
                 * response never alias each other */
                char response[BUFFER_SIZE];
                int written = handle_http_request(ctx->buffer, (size_t)res,
                                                  response, sizeof(response), stats);
                if (written <= 0) {
                    /* Handler failed or produced no output */
                    close_connection(ctx);
                    break;
                }
                if (written >= BUFFER_SIZE) {
                    written = BUFFER_SIZE - 1; /* snprintf truncated the response */
                }

                /* Copy the response into the context buffer, which stays
                 * valid until the asynchronous send completes */
                memcpy(ctx->buffer, response, (size_t)written);
                ctx->total_to_write = (size_t)written;

                if (queue_write(&ring, ctx) < 0) {
                    close_connection(ctx);
                }
                break;
            }

            case EVENT_WRITE:
                /* Response delivered; connections are not kept alive */
                close_connection(ctx);
                break;
            }
        }

        /* Flush any SQEs queued during this iteration to the kernel */
        io_uring_submit(&ring);
    }

    /* ---- Worker teardown ---- */
    io_uring_queue_exit(&ring);
    close(server_sock);
    exit(EXIT_SUCCESS);
}

/* Master                                                              */
/**
 * @brief Fork a single worker process.
 *
 * @param server_sock Listening socket to be inherited by the child
 * @param worker_id   Logical worker index
 * @param stats       Shared-memory metrics block inherited by the child
 * @return The child's PID in the master, or -1 on failure
 */
static pid_t spawn_worker(int server_sock, int worker_id, server_metrics_t *stats) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* Child process: become a worker */
        run_worker_loop(server_sock, worker_id, stats); /* never returns */
    }
    return pid;
}

/**
 * @brief Program entry point: set up the server and supervise the workers
 *
 * Usage: program [port]
 *
 * @param argc Argument count.
 * @param argv Optional port number (1-65535) in argv[1]
 * @return 0 on clean shutdown; exits non-zero on setup failure
 */
int main(int argc, char *argv[]) {
    /* Line-buffer stdout so output buffered before fork() is never duplicated
     * into the children (matters when stdout is a pipe, e.g. docker logs) */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* ---- Argument parsing ---- */
    int port = DEFAULT_PORT;
    if (argc > 1) {
        char *end = NULL;
        long p = strtol(argv[1], &end, 10);
        /* Reject empty strings, trailing garbage, and out-of-range values */
        if (*argv[1] == '\0' || *end != '\0' || p < 1 || p > 65535) {
            fprintf(stderr, "Usage: %s [port]   (port must be 1-65535)\n", argv[0]);
            exit(EXIT_FAILURE);
        }
        port = (int)p;
    }

    /* ---- Listening socket setup ---- */
    /* Listening socket, shared by all workers after fork() */
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* Allow quick restarts without waiting for TIME_WAIT sockets to expire */
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind to all IPv4 interfaces on the requested port */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((uint16_t)port);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    if (listen(server_sock, BACKLOG) < 0) {
        perror("listen");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    /* ---- Shared metrics ---- */
    /* Shared-memory metrics, inherited by every worker via fork() */
    server_metrics_t *stats = init_shared_memory();
    if (!stats) {
        fprintf(stderr, "Failed to initialize shared memory\n");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    printf("[Master PID %d] Listening on port %d with %d pre-forked workers\n",
           getpid(), port, NUM_WORKERS);

    /* ---- Signal configuration ---- */
    /* Install handlers BEFORE forking so no worker starts without them */
    signal(SIGPIPE, SIG_IGN); /* Broken-pipe errors are handled via return codes */
    install_handler(SIGINT, handle_master_signal);
    install_handler(SIGTERM, handle_master_signal);

    /* ---- Supervisor loop ---- */
    /* Supervisor loop: reap dead workers and (re)spawn any missing ones */
    while (server_running) {
        int status;
        pid_t dead;

        /* Non-blocking reap of every worker that has exited since the last pass,
         * clearing its slot so it is respawned below */
        while ((dead = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < NUM_WORKERS; i++) {
                if (worker_pids[i] == dead) {
                    printf("[Master] Worker %d (PID %d) exited\n", i, (int)dead);
                    worker_pids[i] = 0;
                }
            }
        }

        /* Covers both the initial spawn and respawning after a crash */
        for (int i = 0; i < NUM_WORKERS && server_running; i++) {
            if (worker_pids[i] <= 0) {
                worker_pids[i] = spawn_worker(server_sock, i, stats);
            }
        }

        sleep(1); /* interrupted early by SIGINT/SIGTERM */
    }

    /* ---- shutdown ---- */
    printf("\n[Master] Terminating workers and cleaning up...\n");

    /* Ask every live worker to stop */
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (worker_pids[i] > 0) {
            kill(worker_pids[i], SIGTERM);
        }
    }
    /* ...then wait for each one so no zombies or orphans are left behind */
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (worker_pids[i] > 0) {
            waitpid(worker_pids[i], NULL, 0);
        }
    }

    /* Release shared memory / IPC resources, then the listening socket */
    cleanup_ipc();
    close(server_sock);
    return 0;
}