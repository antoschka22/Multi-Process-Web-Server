/**
 * @brief Inter-Process Communication (IPC) management: shared memory and synchronization
 *
 * When child worker processes are spawned via fork(2), the kernel grants them
 * an independent virtual address space via Copy-on-Write (CoW). Changes made to
 * regular heap or global memory in one process are isolated from the others
 *
 * This module establishes:
 * A POSIX shared memory object (shm_open + mmap with MAP_SHARED) to allocate
 *   a physical memory region common to all child workers for live telemetry tracking
 * A named POSIX semaphore (sem_open) configured as a mutual exclusion lock (binary mutex)
 *   to guard concurrent writes against race conditions across processes
 * Lifecycle teardown routines (cleanup_ipc) to release OS-level namespace handles
 */

#include "ipc_manager.h"
#include <sys/mman.h>   /* Memory management declarations (mmap, shm_open, shm_unlink) */
#include <fcntl.h>      /* File control options and open flags (O_CREAT, O_RDWR) */
#include <unistd.h>     /* POSIX operating system API (ftruncate, close) */
#include <stdio.h>      /* Standard I/O routines (perror) */

/**
 * @brief Allocates, sizes, and maps a POSIX shared memory segment for server metrics
 *
 * Procedure:
 * shm_open creates/opens a shared memory object backed by tmpfs (usually in /dev/shm)
 * ftruncate sizes the memory object to exactly hold `server_metrics_t`
 * mmap attaches the kernel memory page to the process address space using MAP_SHARED,
 *   ensuring writes immediately reflect across all other mapped processes
 * The raw file descriptor is closed immediately after mapping, as the active
 *   virtual memory mapping persists independently of the file handle
 * Metric counters are zero-initialized
 *
 * @return Pointer to the mapped `server_metrics_t` struct in shared memory,
 * or NULL if any system call fails
 */
server_metrics_t *init_shared_memory(void) {
    /*
     * Create or open the named POSIX shared memory object
     * Permissions 0666 allow read/write access across matching user/group privileges
     */
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open failed");
        return NULL;
    }

    /*
     * A newly created shared memory object has a length of 0 bytes
     * ftruncate allocates the physical byte backing to fit our metrics structure
     */
    if (ftruncate(shm_fd, sizeof(server_metrics_t)) == -1) {
        perror("ftruncate failed");
        close(shm_fd);
        return NULL;
    }
    
    /*
     * Map the shared memory object into the process virtual address space
     * - PROT_READ | PROT_WRITE: Page permits both reads and writes
     * - MAP_SHARED: Modifications are shared across all processes mapping this region
     */
    server_metrics_t *stats = (server_metrics_t *)mmap(
        NULL,
        sizeof(server_metrics_t), 
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0
    );

    /*
     * The file descriptor is no longer needed once mapped; closing it prevents
     * descriptor leaks while preserving the active virtual address mapping
     */
    close(shm_fd);

    if (stats == MAP_FAILED) {
        perror("mmap failed");
        return NULL;
    }
    
    /*
     * Zero-initialize telemetry metrics in the shared page
     */
    stats->total_requests = 0;
    stats->success_200 = 0;
    stats->error_404 = 0;
    
    return stats;
}

/**
 * @brief Creates or opens a named POSIX semaphore to synchronize shared metrics access
 *
 * Initialized with a value of 1, functioning as a binary semaphore (mutex)
 * Worker processes call sem_wait() before writing to `stats` and sem_post()
 * immediately after to guarantee atomic increments across child processes
 *
 * @return Pointer to the initialized `sem_t` handle, or SEM_FAILED on error
 */
sem_t *init_semaphore(void) {
    /*
     * Named semaphore opened with initial value = 1 (unlocked state)
     * Mode 0666 grants read/write permissions to owner, group, and others
     */
    return sem_open(SEM_NAME, O_CREAT, 0666, 1);
}

/**
 * @brief Removes named IPC handles from the operating system namespace
 *
 * POSIX IPC objects are kernel-persistent and survive process exits. If not unlinked,
 * stale objects remain in `/dev/shm` indefinitely
 *
 * Calling *_unlink:
 * - Removes the name from the system path so subsequent calls won't bind to stale state
 * - Deallocates underlying kernel resources once the last active process
 *   invokes munmap/sem_close or terminates
 */
void cleanup_ipc(void) {
    shm_unlink(SHM_NAME); /* Unlink shared memory entry from the filesystem namespace */
    sem_unlink(SEM_NAME); /* Unlink named semaphore entry from the system table */
}