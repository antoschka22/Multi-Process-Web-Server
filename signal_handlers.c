/**
 * @file signal_handlers.c
 * @brief Rigorous lifecycle and resource management handlers.
 *
 * This module is responsible for keeping the OS clean by asynchronously 
 * intercepting signals from terminating children and keyboard interrupts[cite: 552].
 */

#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include "ipc_manager.h"

/**
 * @brief Intercepts SIGCHLD to safely reap terminating child processes.
 * * Calling the waitpid function inside this asynchronous handler prevents 
 * finished children from becoming resource-draining "zombie" processes.
 */
void handle_sigchld(int sig) {
    (void)sig; 
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/**
 * @brief Gracefully intercepts server termination commands (e.g., Ctrl+C).
 * * Required to ensure the program completely exits without leaving behind 
 * dangling shared memory segments or active semaphores.
 */
void handle_sigint(int sig) {
    (void)sig;
    printf("\nShutting down gracefully...\n");
    
    // Safely unlink and destroy resources before exiting[cite: 555].
    cleanup_ipc();
    exit(0);
}

void setup_sigchld_handler() {
    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    
    // Use SA_RESTART to automatically restart interrupted system calls like accept().
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