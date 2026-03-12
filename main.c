/**
 * @file main.c
 * @brief Master process loop and network socket configuration.
 *
 * This file contains the core lifecycle of the Metric-Aware Web Server. 
 * It sets up the network listener, initializes Inter-Process Communication (IPC) 
 * resources, and implements a concurrent server model using fork() to delegate 
 * client connections to independent child processes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include "http_handler.h"
#include "ipc_manager.h"
#include "signal_handlers.h"
#include "server_stats.h"

#define DEFAULT_PORT 8080
#define BACKLOG 10

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    int server_fd, client_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    int opt = 1;

    /* 1. Initialize Signal Handlers */
    // Equip the parent process with asynchronous signal handlers.
    // setup_sigchld_handler prevents finished children from becoming zombie processes.
    // setup_sigint_handler gracefully intercepts termination to clean up IPC.
    setup_sigchld_handler();
    setup_sigint_handler();

    /* 2. Initialize IPC Resources */
    // Create shared memory and semaphores before the master process starts accepting connections.
    server_metrics_t *stats = init_shared_memory();
    sem_t *sem = init_semaphore();

    if (!stats || !sem) {
        perror("Failed to initialize IPC");
        exit(EXIT_FAILURE);
    }

    /* 3. Setup TCP Socket */
    // Create a TCP socket for the master process.
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    // Forcefully attach socket to the port to avoid "Address already in use" errors.
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Bind the socket to the specific network port.
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    // Listen for incoming client connections.
    if (listen(server_fd, BACKLOG) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", port);

    /* 4. Main Accept Loop */
    while (1) {
        // Accept the connection and receive a dedicated file descriptor.
        client_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (client_fd < 0) {
            perror("Accept failed");
            continue;
        }

        /* 5. Forking Mechanism */
        // Split the execution into two separate processes: parent and child.
        pid_t pid = fork();

        if (pid < 0) {
            perror("Fork failed");
            close(client_fd);
        } else if (pid == 0) {
            /* Child Process Logic */
            close(server_fd); // Child doesn't need the master listener socket
            
            // The newly created child process takes over full responsibility for the client.
            handle_client(client_fd, stats, sem);
            
            close(client_fd);
            exit(0); // Exit child process cleanly
        } else {
            /* Parent Process Logic */
            // The parent process simply closes its copy of the connected client socket.
            close(client_fd); 
            // Loops back immediately to wait for the next incoming connection.
        }
    }

    // Final safety cleanup (typically executed via the SIGINT handler)
    cleanup_ipc();
    return 0;
}