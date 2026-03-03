#ifndef SERVER_STATS_H
#define SERVER_STATS_H

#include <stdint.h>

/* Define the names for the IPC resources */
#define SHM_NAME "/webserver_stats"
#define SEM_NAME "/webserver_sem"

/* * Requirement: A specific data structure containing various live counters.
 */
typedef struct {
    uint64_t total_requests;    /* Total number of incoming requests received  */
    uint64_t success_200;       /* Number of successful 200 OK responses  */
    uint64_t error_404;         /* Number of 404 errors encountered  */
    uint64_t total_bytes_sent;  /* Total aggregate number of bytes transferred  */
} server_metrics_t;

#endif