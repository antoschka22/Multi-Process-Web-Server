/**
 * @brief Master process loop implementing a Pre-Fork Worker Pool architecture.
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

#include "http_handler.h"
#include "ipc_manager.h"
#include "signal_handlers.h"
#include "server_stats.h"

#define DEFAULT_PORT 8080
#define BACKLOG 128
#define NUM_WORKERS 4

/**
 * @brief Tracking array for active worker process IDs.
 */
static pid_t worker_pids[NUM_WORKERS];

/**
 * @brief Volatile flag indicating the operational state of the master process.
 *        Marked sig_atomic_t to ensure safe reads/writes across signal boundaries.
 */
static volatile sig_atomic_t server_running = 1;

/**
 * @brief Signal handler to initiate a graceful shutdown sequence.
 * 
 * Propagates SIGTERM to all registered worker processes before the master exits.
 *
 * @param sig Signal number received (SIGINT or SIGTERM).
 */
static void handle_master_sigint(int sig) {
    (void)sig;
    server_running = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (worker_pids[i] > 0) {
            kill(worker_pids[i], SIGTERM);
        }
    }
}

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    /* 1. IPC Resource Initialization */
    server_metrics_t *stats = init_shared_memory();
    sem_t *sem = init_semaphore();

    if (!stats || !sem) {
        perror("Failed to initialize IPC");
        exit(EXIT_FAILURE);
    }

    /* 2. TCP Socket Initialization and Configuration */
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    /* Allow rapid reuse of local addresses in TIME_WAIT status */
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("====================================================\n");
    printf(" Master process started [PID: %d]\n", getpid());
    printf(" Listening on port: %d (Pool size: %d workers)\n", port, NUM_WORKERS);
    printf("====================================================\n");

    /* 3. Pre-Fork Worker Pool Creation */
    for (int i = 0; i < NUM_WORKERS; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("Fork failed during pool creation");
            exit(EXIT_FAILURE);
        } else if (pid == 0) {
            /* ---------------------------------------------------- */
            /*                 CHILD / WORKER LOOP                  */
            /* ---------------------------------------------------- */
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            while (1) {
                /*
                 * Multiple workers block concurrently on accept().
                 * Modern Linux kernels wake a single worker (mitigating the thundering herd problem).
                 */
                int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                
                if (client_fd < 0) {
                    /* Retry on non-fatal signal interruptions */
                    if (errno == EINTR) {
                        continue;
                    }
                    perror("Worker accept error");
                    break;
                }

                /* Process client request using shared memory metrics protected by semaphore */
                handle_client(client_fd, stats, sem);

                close(client_fd);
            }

            close(server_fd);
            exit(0);
        } else {
            /* Master process: record child PID */
            worker_pids[i] = pid;
            printf(" [Master] Spawned persistent worker [%d] PID: %d\n", i + 1, pid);
        }
    }

    /* 4. Configure Termination Signal Actions for Master */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_master_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 5. Master Supervision Loop: Monitor and supervise child workers */
    while (server_running) {
        int status;
        pid_t dead_child = wait(&status);

        if (dead_child < 0) {
            if (errno == EINTR) {
                /* Interrupted by signal handler (e.g., SIGINT/SIGTERM); evaluate server_running */
                continue;
            }
            break;
        }

        /* Auto-respawn: replace unexpected worker terminations during runtime */
        if (server_running) {
            printf(" [Master] Worker PID %d exited. Respawning replacement...\n", dead_child);
            for (int i = 0; i < NUM_WORKERS; i++) {
                if (worker_pids[i] == dead_child) {
                    pid_t new_pid = fork();
                    if (new_pid == 0) {
                        /* Replacement worker event loop */
                        struct sockaddr_in client_addr;
                        socklen_t client_len = sizeof(client_addr);
                        while (1) {
                            int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                            if (client_fd < 0) {
                                if (errno == EINTR) continue;
                                break;
                            }
                            handle_client(client_fd, stats, sem);
                            close(client_fd);
                        }
                        close(server_fd);
                        exit(0);
                    } else if (new_pid > 0) {
                        worker_pids[i] = new_pid;
                        printf(" [Master] Replacement worker PID: %d created.\n", new_pid);
                    }
                    break;
                }
            }
        }
    }

    /* 6. Teardown and Resource Deallocation */
    printf("\n[Master] Terminating server and cleaning up IPC resources...\n");
    close(server_fd);
    cleanup_ipc();

    return 0;
}