#ifndef HTTP_HANDLER_H
#define HTTP_HANDLER_H

#include "server_stats.h"
#include <semaphore.h>

/* * Requirement: Child process takes over responsibility for dealing with the client[cite: 37].
 * This function parses the GET request and determines the file or URI[cite: 38].
 */
void handle_client(int client_socket, server_metrics_t *stats, sem_t *sem);

/* * Requirement: Special endpoint mapped to "/status" to display live dashboard[cite: 52].
 * Generates dynamic HTML/JSON from shared memory values[cite: 54].
 */
void serve_status_page(int client_socket, server_metrics_t *stats);

/* * Requirement: Serve regular files (200 OK) or missing files (404 Not Found)[cite: 39, 40].
 */
void serve_file(int client_socket, const char *path);

/**
 * @brief Parses an HTTP GET request and prepares the HTTP response.
 *
 * @param request_buf Raw incoming request buffer from io_uring.
 * @param req_len Number of bytes received.
 * @param response_buf Output buffer to write the HTTP response into.
 * @param max_resp_len Maximum capacity of the output buffer.
 * @return int Total bytes written to response_buf, or -1 on error.
 */
int handle_http_request(const char *request_buf, size_t req_len, char *response_buf, size_t max_resp_len, server_metrics_t *stats);
#endif