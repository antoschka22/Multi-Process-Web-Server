#ifndef SIGNAL_HANDLERS_H
#define SIGNAL_HANDLERS_H

/* * Requirement: Asynchronous signal handler for SIGCHLD to prevent "zombie" processes[cite: 56].
 * Should use waitpid() inside to clean up finished children.
 */
void setup_sigchld_handler();

/* * Requirement: Gracefully intercept Ctrl+C (SIGINT) to destroy IPC resources[cite: 57, 58].
 */
void setup_sigint_handler();

#endif