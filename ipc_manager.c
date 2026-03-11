#include "ipc_manager.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

server_metrics_t* init_shared_memory() {
    // Create shared memory segment [cite: 78]
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, sizeof(server_metrics_t));
    
    server_metrics_t* stats = (server_metrics_t*)mmap(NULL, sizeof(server_metrics_t), 
                              PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    // Initialize counters to zero
    stats->total_requests = 0;
    stats->success_200 = 0;
    stats->error_404 = 0;
    
    return stats;
}

sem_t* init_semaphore() {
    // Initialize named semaphore
    return sem_open(SEM_NAME, O_CREAT, 0666, 1);
}

void cleanup_ipc() {
    // Unlink resources on exit
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_NAME);
}