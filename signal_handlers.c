/**
 * @brief Asynchronous process lifecycle and OS resource reclamation handlers
 *
 * This module configures signal disposition using POSIX sigaction(2) to manage
 * two critical operational requirements in a preforked / multi-process server:
 *
 * Child Process Reclamation (SIGCHLD):
 *    Asynchronously reaps terminated child worker processes using a non-blocking
 *    waitpid() loop, ensuring terminated processes do not persist in the process
 *    table as zombies
 *
 * Graceful Shutdown & IPC Teardown (SIGINT / SIGTERM):
 *    Intercepts manual interrupt signals (e.g., Ctrl+C) to run cleanup_ipc(),
 *    unlinking POSIX shared memory segments and semaphores from /dev/shm before
 *    process exit
 */

#include <sys/wait.h>   /* Declarations for waitpid(), WNOHANG */
#include <signal.h>     /* POSIX signal handling interfaces (sigaction, sigemptyset) */
#include <stdlib.h>     /* Standard process exit facilities (exit, EXIT_SUCCESS) */
#include <stdio.h>      /* Standard I/O operations (printf) */
#include <errno.h>      /* System errno definitions (saved errno preservation) */
#include "ipc_manager.h"/* IPC teardown prototype (cleanup_ipc) */

/**
 * @brief Asynchronous signal handler for SIGCHLD to reap terminated child processes
 *
 * When a child process terminates, the kernel delivers SIGCHLD to the parent
 * Multiple children can terminate near-simultaneously; because POSIX standard signals
 * do not queue, a single SIGCHLD invocation may represent multiple terminated children
 *
 * Therefore, waitpid() is invoked inside a while loop with:
 * - pid = -1: Reap any terminated child process
 * - WNOHANG: Return immediately if no more child state changes exist (non-blocking)
 *
 * @note Preserving errno is standard practice in signal handlers to avoid
 *       clobbering an active system call's error state in the main thread
 *
 * @param sig Signal number received (SIGCHLD); cast to void to silence unused parameter warnings
 */
void handle_sigchld(int sig) {
    (void)sig;

    /* Preserve errno across the signal handler execution context */
    int saved_errno = errno;

    /*
     * Drain the queue of terminated child processes
     * Loop continues until waitpid() returns 0 (no more pending child state changes)
     * or -1 (no child processes remain, errno == ECHILD)
     */
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        /* Intentionally empty body: child exit status is reaped and discarded */
    }

    errno = saved_errno;
}

/**
 * @brief Asynchronous signal handler for SIGINT (interactive interrupt / Ctrl+C)
 *
 * Traps termination requests to guarantee that IPC resources (shared memory objects
 * and named semaphores) are unlinked before the process terminates. Without this step,
 * persistent kernel objects in tmpfs (/dev/shm) would remain orphaned
 *
 * @param sig Signal number received (SIGINT); cast to void to silence unused parameter warnings
 */
void handle_sigint(int sig) {
    (void)sig;

    /*
     * Inform operator via console
     */
    printf("\nShutting down gracefully...\n");

    /*
     * Unlink POSIX shared memory and semaphores from the system namespace
     * Prevents dangling resources across server runs
     */
    cleanup_ipc();

    /* Terminate the parent process normally */
    exit(0);
}

/**
 * @brief Registers the asynchronous SIGCHLD handler via sigaction(2)
 *
 * Configuration highlights:
 * - sigemptyset: Clears the signal mask so no extra signals are blocked during handling
 * - SA_RESTART: Automatically restarts slow blocking system calls (e.g., accept(), read())
 *   if they are interrupted by SIGCHLD, preventing spurious EINTR errors in the master accept loop
 * - SA_NOCLDSTOP: Disables SIGCHLD generation when children are stopped (SIGSTOP) or resumed (SIGCONT),
 *   triggering only upon true child termination
 */
void setup_sigchld_handler(void) {
    struct sigaction sa;

    /* Assign custom signal handler function */
    sa.sa_handler = handle_sigchld;

    /* Do not block additional signals during handler execution */
    sigemptyset(&sa.sa_mask);

    /*
     * SA_RESTART: Restart interrupted system calls (e.g., accept(2)).
     * SA_NOCLDSTOP: Ignore job control signals like SIGSTOP/SIGCONT from children
     */
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    /* Install the handler for SIGCHLD */
    sigaction(SIGCHLD, &sa, NULL);
}

/**
 * @brief Registers the graceful shutdown handler for SIGINT via sigaction(2)
 *
 * Configures the execution context for SIGINT (terminal interrupt). Unlike SIGCHLD,
 * SA_RESTART is omitted here so blocking calls can be aborted cleanly to expedite shutdown
 */
void setup_sigint_handler(void) {
    struct sigaction sa;

    /* Assign graceful termination routine */
    sa.sa_handler = handle_sigint;

    /* Clear signal mask */
    sigemptyset(&sa.sa_mask);

    /*
     * Flags set to 0: do not restart interrupted system calls,
     * allowing blocking I/O to unblock and terminate promptly
     */
    sa.sa_flags = 0;

    /* Install the handler for SIGINT */
    sigaction(SIGINT, &sa, NULL);
}