/**
 * @file ipc_manager.c
 * @brief Initialization and teardown of shared memory and synchronization tools.
 *
 * Because child processes spawned by fork have their own independent memory 
 * spaces, this module uses mmap and shm_open to allow isolated processes to 
 * share and track global variables seamlessly[cite: 540, 541].
 */

#include "ipc_manager.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

server_metrics_t* init_shared_memory() {
    // Initialize a shared memory segment using standard system calls[cite: 541].
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, sizeof(server_metrics_t));
    
    // Map the shared memory block into the process address space.
    server_metrics_t* stats = (server_metrics_t*)mmap(NULL, sizeof(server_metrics_t), 
                              PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    // Initialize the specific data structure containing various live counters[cite: 542].
    stats->total_requests = 0;
    stats->success_200 = 0;
    stats->error_404 = 0;
    
    return stats;
}

sem_t* init_semaphore() {
    // Implement strict synchronization using POSIX semaphores to act as a lock.
    return sem_open(SEM_NAME, O_CREAT, 0666, 1);
}

void cleanup_ipc() {
    // Safely unlink and destroy the shared memory segment and semaphores[cite: 555].
    // This ensures no memory leaks or dangling resources are left behind in the OS[cite: 555].
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_NAME);
}