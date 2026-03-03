#ifndef IPC_MANAGER_H
#define IPC_MANAGER_H

#include "server_stats.h"
#include <semaphore.h>

/* * Requirement: Initialize shared memory before accepting connections[cite: 44].
 * Returns a pointer to the shared memory segment.
 */
server_metrics_t* init_shared_memory();

/* * Requirement: Implement strict synchronization using POSIX semaphores[cite: 49].
 * Returns a pointer to the named semaphore.
 */
sem_t* init_semaphore();

/* * Requirement: Safely increments memory counters using a lock[cite: 50, 51].
 */
void update_metrics(server_metrics_t *stats, sem_t *sem, int status_code, size_t bytes);

/* * Requirement: Safely unlink and destroy resources on termination[cite: 58].
 */
void cleanup_ipc();

#endif