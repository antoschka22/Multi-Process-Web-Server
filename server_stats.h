#ifndef SERVER_STATS_H
#define SERVER_STATS_H

#include <stdint.h>     /* Fixed-width integer types */
#include <stdatomic.h>  /* C11 atomic types and memory ordering operations */

/**
 * @brief Global telemetry metrics structure and IPC namespace identifiers
 *
 * Defines the shared state layout mapped into shared memory (`/dev/shm`)
 * across all worker child processes and threads
 */

/**
 * @def SHM_NAME
 * @brief System-wide identifier for the POSIX shared memory object
 *
 * Backed by tmpfs in `/dev/shm/webserver_stats` on Linux. Must begin with
 * a leading slash (`/`) per POSIX standard naming conventions
 */
#define SHM_NAME "/webserver_stats"

/**
 * @def SEM_NAME
 * @brief System-wide identifier for the named POSIX semaphore
 *
 * Controls synchronized access across processes where atomic operations
 * are supplemented by semaphore-level critical sections
 */
#define SEM_NAME "/webserver_sem"

/**
 * @brief Shared live server metrics and telemetry counters
 *
 * Allocated in a shared memory region (`MAP_SHARED`) to track live request
 * activity across independent processes
 *
 * Fields use C11 atomic primitives (`atomic_uint_least64_t`) to provide:
 * Native lock-free reads and writes (e.g., using `atomic_fetch_add`)
 * Protection against torn reads/writes across 64-bit boundaries
 * Cache-coherent synchronization across CPU cores without requiring
 *   coarse-grained lock acquisition for individual counter increments
 */
typedef struct {
    atomic_uint_least64_t total_requests;    /**< Cumulative HTTP requests received and dispatched */
    atomic_uint_least64_t success_200;       /**< Total HTTP 200 OK responses served */
    atomic_uint_least64_t error_404;         /**< Total HTTP 404 Not Found responses dispatched */
    atomic_uint_least64_t total_bytes_sent;  /**< Aggregated network payload volume transmitted (bytes) */
} server_metrics_t;

#endif /* SERVER_STATS_H */