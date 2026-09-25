#ifndef SERVER_STATS_H
#define SERVER_STATS_H

#include <stdint.h>
#include <stdatomic.h>

#define SHM_NAME "/webserver_stats"
#define SEM_NAME "/webserver_sem"

// Requirement: A specific data structure containing various live counters.
typedef struct {
    atomic_uint_least64_t total_requests;
    atomic_uint_least64_t success_200;
    atomic_uint_least64_t error_404;
    atomic_uint_least64_t total_bytes_sent;
} server_metrics_t;

#endif