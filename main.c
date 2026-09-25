/**
 * @brief   ster process loop implementing a Pre-Fork Worker Pool architecture.
 *
 * This module sets up the TCP listening socket, initializes Inter-Process
 * Communication (IPC) resources, and manages a persistent pool of child worker
 * processes that concurrently accept connections on the shared server socket.
 * It also provides fault tolerance via process supervision and automated child respawning.
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
#include <fcntl.h>
#include <liburing.h>

#include "uring_server.h"
#include "http_handler.h"
#include "signal_handlers.h"
#include "server_stats.h"

#define DEFAULT_PORT 8080
#define BACKLOG 512
#define NUM_WORKERS 4

static pid_t worker_pids[NUM_WORKERS];
static volatile sig_atomic_t server_running = 1;

/* Signal handler for graceful master shutdown */
static void handle_master_sigint(int sig) {
    (void)sig;
    server_running = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (worker_pids[i] > 0) {
            kill(worker_pids[i], SIGTERM);
        }
    }
}

/* Worker signal handler to break the io_uring loop */
static void handle_worker_sigterm(int sig) {
    (void)sig;
    server_running = 0;
}

/* Helper: submit async accept to the ring */
static void add_accept(struct io_uring *ring, int server_sock, struct sockaddr_in *client_addr, socklen_t *client_len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_accept(sqe, server_sock, (struct sockaddr *)client_addr, client_len, 0);
    
    conn_context_t *ctx = malloc(sizeof(conn_context_t));
    ctx->fd = server_sock;
    ctx->event_type = EVENT_ACCEPT;
    
    io_uring_sqe_set_data(sqe, ctx);
}

/* Helper: submit async read to the ring */
static void add_read(struct io_uring *ring, int client_fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    
    conn_context_t *ctx = malloc(sizeof(conn_context_t));
    ctx->fd = client_fd;
    ctx->event_type = EVENT_READ;
    ctx->bytes_transferred = 0;
    
    io_uring_prep_read(sqe, client_fd, ctx->buffer, BUFFER_SIZE - 1, 0);
    io_uring_sqe_set_data(sqe, ctx);
}

/* Helper: submit async write to the ring */
static void add_write(struct io_uring *ring, conn_context_t *ctx) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    
    ctx->event_type = EVENT_WRITE;
    io_uring_prep_write(sqe, ctx->fd, ctx->buffer, ctx->total_to_write, 0);
    io_uring_sqe_set_data(sqe, ctx);
}

/**
 * @brief Worker event loop powered by io_uring
 */
static void run_worker_loop(int server_sock, int worker_id) {
    struct io_uring ring;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    // Setup worker-specific signal handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_worker_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    int written = handle_http_request(ctx->buffer, res, ctx->buffer, BUFFER_SIZE, stats);

    // Initialize io_uring with Kernel Submission Polling (SQPOLL)
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000; // Milliseconds before kernel thread sleeps if idle

    if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params) < 0) {
        perror("io_uring_queue_init_params (SQPOLL)");
        exit(EXIT_FAILURE);
    }

    printf("[Worker %d (PID %d)] io_uring SQPOLL ring initialized\n", worker_id, getpid());

    // Queue first accept operation
    add_accept(&ring, server_sock, &client_addr, &client_len);
    io_uring_submit(&ring);

    // Worker Event Loop
    while (server_running) {
        struct io_uring_cqe *cqe;
        
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue; // Interrupted by shutdown signal
            perror("io_uring_wait_cqe");
            break;
        }

        conn_context_t *ctx = (conn_context_t *)io_uring_cqe_get_data(cqe);
        int res = cqe->res;

        if (res < 0) {
            // An I/O error occurred on this entry
            if (ctx->event_type != EVENT_ACCEPT) {
                close(ctx->fd);
                free(ctx);
            }
            io_uring_cqe_seen(&ring, cqe);
            continue;
        }

        switch (ctx->event_type) {
            case EVENT_ACCEPT: {
                int client_fd = res;
                
                // Read from the newly connected socket
                add_read(&ring, client_fd);

                // Re-queue the accept listener to keep accepting new connections
                add_accept(&ring, server_sock, &client_addr, &client_len);
                free(ctx);
                break;
            }

            case EVENT_READ: {
                if (res == 0) {
                    // Client disconnected (EOF)
                    close(ctx->fd);
                    free(ctx);
                } else {
                    ctx->buffer[res] = '\0';

                    // Delegate HTTP parsing and response assembly to http_handler
                    // Expecting handle_http_request to populate ctx->buffer or return bytes written
                    int written = handle_http_request(ctx->buffer, res, ctx->buffer, BUFFER_SIZE);
                    
                    if (written > 0) {
                        ctx->total_to_write = written;
                        add_write(&ring, ctx);
                    } else {
                        close(ctx->fd);
                        free(ctx);
                    }
                }
                break;
            }

            case EVENT_WRITE: {
                // Response delivered; close connection and free context
                close(ctx->fd);
                free(ctx);
                break;
            }
        }

        io_uring_cqe_seen(&ring, cqe);
        io_uring_submit(&ring);
    }

    io_uring_queue_exit(&ring);
    close(server_sock);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    // Setup Listening Socket with SO_REUSEPORT (Kernel level round-robin across workers)
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

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

    printf("[Master PID %d] Listening on port %d with %d pre-forked workers\n", 
           getpid(), port, NUM_WORKERS);

    // Initialize POSIX Shared Memory
    server_metrics_t *stats = init_shared_memory();
    if (!stats) {
        fprintf(stderr, "Failed to initialize shared memory\n");
        exit(EXIT_FAILURE);
    }

    // Register signals and fork workers
    for (int i = 0; i < NUM_WORKERS; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            run_worker_loop(server_sock, i, stats);
        } else {
            worker_pids[i] = pid;
        }
    }

    // 2. Register Master Signal Handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_master_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 3. Fork Worker Processes
    for (int i = 0; i < NUM_WORKERS; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        } else if (pid == 0) {
            // Child Worker: Enters io_uring event loop
            run_worker_loop(server_sock, i);
        } else {
            // Master: Track child PID
            worker_pids[i] = pid;
        }
    }

    // 4. Master Process Supervisor Loop
    while (server_running) {
        int status;
        pid_t dead_worker = waitpid(-1, &status, WNOHANG);

        if (dead_worker > 0) {
            for (int i = 0; i < NUM_WORKERS; i++) {
                if (worker_pids[i] == dead_worker) {
                    printf("[Master] Worker %d (PID %d) died. Respawning...\n", i, dead_worker);
                    pid_t new_pid = fork();
                    if (new_pid == 0) {
                        run_worker_loop(server_sock, i);
                    } else {
                        worker_pids[i] = new_pid;
                    }
                }
            }
        }
        sleep(1);
    }

    // 5. Cleanup and Graceful Exit
    printf("\n[Master] Terminating workers and cleaning up...\n");
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (worker_pids[i] > 0) {
            waitpid(worker_pids[i], NULL, 0);
        }
    }

    cleanup_ipc();
    close(server_sock);
    return 0;
}