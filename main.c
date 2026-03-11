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

    // 1. Initialize Signal Handlers
    // setup_sigchld_handler prevents zombie processes
    // setup_sigint_handler ensures IPC cleanup on Ctrl+C
    setup_sigchld_handler();
    setup_sigint_handler();

    // 2. Initialize IPC Resources
    // Create shared memory and semaphores before accepting connections
    server_metrics_t *stats = init_shared_memory();
    sem_t *sem = init_semaphore();

    if (!stats || !sem) {
        perror("Failed to initialize IPC");
        exit(EXIT_FAILURE);
    }

    // 3. Setup TCP Socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    // Forcefully attaching socket to the port
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Bind and Listen
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", port);

    // 4. Main Accept Loop
    while (1) {
        client_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (client_fd < 0) {
            perror("Accept failed");
            continue;
        }

        // 5. Forking Mechanism
        pid_t pid = fork();

        if (pid < 0) {
            perror("Fork failed");
            close(client_fd);
        } else if (pid == 0) {
            // Child Process logic
            close(server_fd); // Child doesn't need the listener
            
            // Handle the client request
            // This includes parsing GET, serving files, or the /status page
            handle_client(client_fd, stats, sem);
            
            close(client_fd);
            exit(0); // Exit child process
        } else {
            // Parent Process logic
            close(client_fd); // Parent doesn't need this specific client socket
            // Loop back to accept the next connection
        }
    }

    // Final safety cleanup (usually reached via SIGINT handler)
    cleanup_ipc();
    return 0;
}