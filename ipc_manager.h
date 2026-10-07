#ifndef IPC_MANAGER_H
#define IPC_MANAGER_H

#include "server_stats.h" /* Shared metrics structures (server_metrics_t) across processes */
#include <semaphore.h>    /* POSIX named and unnamed semaphore definitions (sem_t) */
#include <stddef.h>       /* Standard definitions for size_t */

/**
 * @brief Public interface for inter-process communication (IPC) and synchronization
 *
 * This module provides the lifecycle and synchronization primitives needed for
 * multi-process concurrency:
 * Establishes a POSIX shared memory region (`shm_open`, `mmap`) to share
 *    telemetry and counters across worker processes spawned via `fork()`
 * Manages a POSIX semaphore (`sem_open`) acting as a mutual exclusion lock
 *    to prevent concurrent write races
 * Provides thread-/process-safe counter updates and OS-level namespace teardown
 */

/**
 * @brief Initializes and maps the shared memory segment for global server metrics
 *
 * Must be invoked by the primary server process prior to accepting connections
 * or forking worker processes. Creates (or attaches to) the backing shared memory
 * object, sizes it to fit `server_metrics_t`, and maps it with `MAP_SHARED`
 *
 * @return Pointer to the mapped `server_metrics_t` structure in the caller's
 *         virtual address space, or NULL if initialization/mapping failed
 */
server_metrics_t *init_shared_memory(void);

/**
 * @brief Initializes or connects to the named POSIX semaphore
 *
 * Configured as a binary semaphore (initial value = 1) to synchronize read/write
 * critical sections against the shared metrics segment across separate processe
 *
 * @return Pointer to the initialized `sem_t` handle, or SEM_FAILED on error
 */
sem_t *init_semaphore(void);

/**
 * @brief Safely updates shared metrics under mutual exclusion.
 *
 * Acquires the semaphore (`sem_wait`), increments request/status/byte counters
 * based on the completed transaction, and releases the lock (`sem_post`)
 *
 * @param stats       Pointer to the active mapped shared metrics struct
 * @param sem         Pointer to the POSIX semaphore guarding access to @p stats.
 * @param status_code HTTP status code of the completed response (e.g., 200, 404)
 * @param bytes       Total payload bytes written to the client connection
 */
void update_metrics(server_metrics_t *stats, sem_t *sem, int status_code, size_t bytes);

/**
 * @brief Unlinks shared IPC resources from the operating system namespace
 *
 * Should be called by the master process during graceful server shutdown
 * Unlinks the shared memory identifier (`shm_unlink`) and semaphore name (`sem_unlink`),
 * ensuring kernel-level backing resources are cleaned up once all processes close them
 */
void cleanup_ipc(void);

#endif /* IPC_MANAGER_H */