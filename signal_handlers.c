#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include "ipc_manager.h"

// Logic to prevent zombie processes
void handle_sigchld(int sig) {
    (void)sig; 
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

// Logic for graceful shutdown and IPC cleanup
void handle_sigint(int sig) {
    (void)sig;
    printf("\nShutting down gracefully...\n");
    cleanup_ipc();
    exit(0);
}

void setup_sigchld_handler() {
    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

void setup_sigint_handler() {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}