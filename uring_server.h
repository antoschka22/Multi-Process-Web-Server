#ifndef URING_SERVER_H
#define URING_SERVER_H

#include <liburing.h>
#include <stddef.h>
#include "server_stats.h"

#define QUEUE_DEPTH 512
#define BUFFER_SIZE 4096

typedef enum {
    EVENT_ACCEPT,
    EVENT_READ,
    EVENT_WRITE
} event_type_t;

typedef struct {
    int fd;
    event_type_t event_type;
    char buffer[BUFFER_SIZE];
    size_t bytes_transferred;
    size_t total_to_write;
} conn_context_t;

#endif