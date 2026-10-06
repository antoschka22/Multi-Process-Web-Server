#ifndef HTTP_HANDLER_H
#define HTTP_HANDLER_H

#include "server_stats.h" /* Shared metrics structures (server_metrics_t) across processes/threads */
#include <semaphore.h>    /* POSIX semaphore definitions (sem_t) for synchronizing shared state */
#include <stddef.h>       /* Standard definitions for size_t */

/**
 * @brief Core HTTP request dispatching, file serving, and metrics interfaces
 *
 * This module defines the contract for handling client connections in both
 * multi-process/threaded architectures and asynchronous I/O frameworks (e.g., io_uring)
 * It manages protocol parsing, metric collection, dynamic status page rendering,
 * and static file delivery
 */

/**
 * @brief Primary entry point for handling a connected client socket
 *
 * Typically invoked by a worker child process or thread after accepting a connection
 * Responsibilities include:
 * Reading and parsing the incoming HTTP request line (e.g., method and URI)
 * Updating global server counters safely using the provided POSIX semaphore
 * Routing the request to static file delivery, dynamic status rendering,
 *   or error dispatchers (e.g., 400 Bad Request, 404 Not Found, 405 Method Not Allowed)
 * Closing the socket or prepping it for keep-alive before termination
 *
 * @param client_socket Open and connected socket descriptor for the client
 * @param stats         Pointer to shared memory metrics tracked across worker instances
 * @param sem           POSIX semaphore protecting concurrent writes to @p stats
 */
void handle_client(int client_socket, server_metrics_t *stats, sem_t *sem);

/**
 * @brief Renders and serves the dynamic diagnostic/status dashboard
 *
 * Mapped to administrative endpoints (such as "/status"). Reads live telemetry
 * from the shared memory segment and formats the data into an HTTP response payload
 * (HTML dashboard or JSON object) before writing directly to the client socket
 *
 * @param client_socket Open socket descriptor to stream the generated payload to
 * @param stats         Read-only reference to live server metrics
 */
void serve_status_page(int client_socket, server_metrics_t *stats);

/**
 * @brief Transmits a local disk file or returns an appropriate error page
 *
 * Resolves the sanitized filesystem path, probes file existence/read permissions,
 * and transmits either:
 * - A 200 OK header followed by file content (using standard write/sendfile)
 * - A 404 Not Found (or 403 Forbidden) HTTP response if unavailable
 *
 * @note Implementations must perform path canonicalization to prevent
 *       directory traversal vulnerabilities (e.g., blocking `../` escapes)
 *
 * @param client_socket Open socket descriptor connected to the recipient
 * @param path          Resolved local relative or absolute filepath to serve
 */
void serve_file(int client_socket, const char *path);

/**
 * @brief Parses an HTTP GET request in-memory and constructs the full response frame
 *
 * Designed for asynchronous/non-blocking event loops (such as io_uring or epoll)
 * where socket reads and writes are staged in user-provided memory buffers rather
 * than performed synchronously via blocking socket calls
 *
 * @param request_buf   Raw buffer containing bytes received from the network layer
 * @param req_len       Length in bytes of valid data inside @p request_buf
 * @param response_buf  Destination buffer where headers and payload will be assembled
 * @param max_resp_len  Capacity bound of @p response_buf to prevent buffer overflow
 * @param stats         Metrics handle for tracking request counts, errors, and throughput.
 
 * @return Number of formatted bytes populated into @p response_buf ready for transmission,
 * or -1 on malformed request or internal parsing errors
 */
int handle_http_request(const char *request_buf,
                        size_t req_len,
                        char *response_buf,
                        size_t max_resp_len,
                        server_metrics_t *stats);

#endif /* HTTP_HANDLER_H */