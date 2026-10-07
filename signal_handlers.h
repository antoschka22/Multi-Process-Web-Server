#ifndef SIGNAL_HANDLERS_H
#define SIGNAL_HANDLERS_H

/**
 * @brief Public interface for asynchronous process signals and lifecycle hooks.
 *
 * Configures signal dispositions using POSIX sigaction(2) to handle two core
 * operational requirements in a multi-process, preforked HTTP server:
 *
 * Child Process Reclamation:
 *    Asynchronously reaps terminating child worker processes upon receipt of
 *    SIGCHLD, preventing dead processes from consuming entries in the kernel's
 *    process table as "zombies."
 *
 * Graceful Shutdown & Resource Cleanup:
 *    Traps interactive termination signals (such as SIGINT / Ctrl+C) to run
 *    IPC teardown routines before process exit, preventing orphaned POSIX
 *    shared memory segments and named semaphores in the OS
 */

/**
 * @brief Registers the asynchronous signal handler for SIGCHLD
 *
 * Configures a non-blocking waitpid() loop (`WNOHANG`) to harvest exit statuses
 * of terminating child processes without stalling the parent
 *
 * Operational highlights:
 * - Uses `SA_RESTART` so blocking system calls (e.g., `accept()`) interrupted
 *   by child exits resume automatically rather than failing with `EINTR`
 * - Uses `SA_NOCLDSTOP` to ignore job control signals (`SIGSTOP`/`SIGCONT`),
 *   reaping only on actual child process termination
 */
void setup_sigchld_handler(void);

/**
 * @brief Registers the asynchronous termination handler for SIGINT (Ctrl+C)
 *
 * Traps terminal interrupts to trigger an orderly server shutdown sequence
 * Ensures critical IPC teardown functions (`cleanup_ipc()`) are executed to
 * unlink shared memory objects and semaphores from `/dev/shm` before the parent
 * process calls `exit()`
 */
void setup_sigint_handler(void);

#endif /* SIGNAL_HANDLERS_H */