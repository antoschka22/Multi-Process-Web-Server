#define _GNU_SOURCE  /* Exposes Linux/glibc-specific extensions, including sendfile() and TCP_CORK */

#include <stdio.h>       /* Standard I/O operations (snprintf, perror) */
#include <stdlib.h>      /* Standard library utilities */
#include <string.h>      /* String manipulation functions */
#include <unistd.h>      /* Standard POSIX operating system API (close) */
#include <fcntl.h>       /* File control options (open flags like O_RDONLY) */
#include <sys/stat.h>    /* File status and attributes structures (fstat, struct stat) */
#include <sys/types.h>   /* Primitive system data types (off_t, ssize_t) */
#include <sys/socket.h>  /* Core socket programming interfaces (send, setsockopt) */
#include <sys/sendfile.h>/* Linux-specific zero-copy data transfer interface (sendfile) */
#include <netinet/tcp.h> /* TCP-level socket options (TCP_CORK) */
#include <errno.h>       /* System error number definitions (EAGAIN, EINTR) */

#if defined(__linux__)
#include <sys/sendfile.h>
#endif


/**
 * @brief Serves a static file to an active client socket using zero-copy I/O
 *
 * This function bypasses userspace data transfers entirely:
 * Reads file metadata via fstat() to construct the Content-Length heade
 * Sends HTTP response headers over standard socket write channels
 * Engages TCP_CORK to force packet coalescing (optimizing throughput)
 * Invokes sendfile(2) to stream file contents directly from the OS page
 *  cache to the network socket descriptor within kernel space
 * Disables TCP_CORK to flush pending frames and closes the opened file
 *
 * @param client_socket  File descriptor for the connected client socket
 * @param file_path      Absolute or relative path to the static file on disk
 * @param content_type   MIME type string (e.g., "text/html", "image/png")
 *
 * @return 0 on complete transmission, -1 on any system or I/O failure
 */
int serve_file_zero_copy(int client_socket, const char *file_path, const char *content_type) {
    /*
     * Open the requested static asset in read-only mode
     * We need a valid file descriptor to probe file metadata and back sendfile()
     */
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        return -1;
    }

    /*
     * Retrieve file status metadata
     * st_size is required to inform the client of exact payload dimensions
     * via the HTTP Content-Length header, and to drive our transfer loop bound
     */
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        return -1;
    }

    off_t total_bytes = st.st_size;

    /*
     * Construct standard HTTP/1.1 response headers
     * 512 bytes is adequate for this fixed header payload; snprintf ensures
     * buffer overflow protection
     */
    char header_buffer[512];
    int header_len = snprintf(header_buffer, sizeof(header_buffer),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "Server: C-ZeroCopy-Server/1.0\r\n"
        "\r\n",
        content_type, (long)total_bytes);

    /*
     * Transmit HTTP headers to the client
     * At this stage, data is copied from our userspace buffer to the socket buffer
     */
    if (send(client_socket, header_buffer, header_len, 0) < 0) {
        close(file_fd);
        return -1;
    }

    /*
     * Enable TCP_CORK (Linux-specific optimization)
     *
     * Tells the TCP stack to accumulate outgoing data into full MSS (Maximum
     * Segment Size) packets rather than transmitting partial frames immediately
     * This prevents sending the HTTP headers in an isolated tiny packet and
     * merges subsequent file data into complete MTU-sized packets
     */
    int cork = 1;
    setsockopt(client_socket, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));

    /*
     * Stream file contents directly from disk cache to the network socket
     *
     * sendfile(out_fd, in_fd, &offset, count):
     * - Operates entirely within kernel space (zero-copy), eliminating costly
     *   context switches and dual-copying (kernel -> user buffer -> kernel)
     * - Automatically advances 'offset' by the count of bytes successfully sent
     */
    off_t offset = 0;
    while (offset < total_bytes) {
        size_t bytes_to_send = (size_t)(total_bytes - offset);
        ssize_t bytes_sent = sendfile(client_socket, file_fd, &offset, bytes_to_send);

        if (bytes_sent < 0) {
            /*
             * Handle transient non-fatal errors:
             * - EINTR: Call was interrupted by a signal before data was sent
             * - EAGAIN / EWOULDBLOCK: Socket send buffer is full in non-blocking mode
             */
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }

            perror("sendfile failed");
            close(file_fd);
            return -1;
        }

        if (bytes_sent == 0) {
            /*
             * Premature EOF reached (e.g., file was truncated concurrently
             * during transmission). Break out to avoid an infinite loop
             */
            break;
        }
    }

    /*
     * Disable TCP_CORK to flush any remaining buffered/partial packet data
     * immediately onto the wire
     */
    cork = 0;
    setsockopt(client_socket, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));

    /* Release file descriptor resources; client socket management remains with caller */
    close(file_fd);
    return total_bytes + header_len;
}

/**
 * Sirve páginas de error 404.
 */
static void serve_404(int client_socket) {
    const char *not_found_body = 
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 48\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html><body><h1>404 Not Found</h1></body></html>";
    send(client_socket, not_found_body, strlen(not_found_body), 0);
}

/**
 * Sirve la página de estadísticas dinámicas /status.
 */
void serve_status_page(int client_socket, server_metrics_t *stats) {
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<html><head><title>Server Status</title></head><body>"
        "<h1>Server Live Metrics</h1>"
        "<ul>"
        "<li>Total Requests: %llu</li>"
        "<li>200 OK Responses: %llu</li>"
        "<li>404 Errors: %llu</li>"
        "<li>Total Bytes Sent: %llu</li>"
        "</ul></body></html>",
        (unsigned long long)stats->total_requests,
        (unsigned long long)stats->success_200,
        (unsigned long long)stats->error_404,
        (unsigned long long)stats->total_bytes_sent);

    char response[1024];
    int resp_len = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        body_len, body);

    send(client_socket, response, resp_len, 0);
}


void serve_file(int client_socket, const char *path) {
    const char *mime = get_mime_type(path);
    if (serve_file_zero_copy(client_socket, path, mime) < 0) {
        serve_404(client_socket);
    }
}


void handle_client(int client_socket, server_metrics_t *stats, sem_t *sem) {
    char buffer[2048];
    ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }
    buffer[bytes_read] = '\0';

    // Parseo básico de método y URI
    char method[16], uri[256];
    if (sscanf(buffer, "%15s %255s", method, uri) < 2) {
        close(client_socket);
        return;
    }

    if (strcmp(method, "GET") != 0) {
        const char *not_implemented = "HTTP/1.1 501 Not Implemented\r\nConnection: close\r\n\r\n";
        send(client_socket, not_implemented, strlen(not_implemented), 0);
        close(client_socket);
        return;
    }

    // Ruta especial /status
    if (strcmp(uri, "/status") == 0) {
        if (sem) sem_wait(sem);
        if (stats) stats->total_requests++;
        serve_status_page(client_socket, stats);
        if (stats) stats->success_200++;
        if (sem) sem_post(sem);
        close(client_socket);
        return;
    }

    // Mapeo a archivos estáticos dentro del directorio public/ o raíz
    char file_path[512];
    if (strcmp(uri, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "index.html");
    } else {
        // Omite el '/' inicial
        snprintf(file_path, sizeof(file_path), "public%s", uri);
    }

    const char *mime = get_mime_type(file_path);
    int sent_bytes = serve_file_zero_copy(client_socket, file_path, mime);

    // Registro sincronizado de métricas en memoria compartida
    if (sem) sem_wait(sem);
    if (stats) {
        stats->total_requests++;
        if (sent_bytes >= 0) {
            stats->success_200++;
            stats->total_bytes_sent += sent_bytes;
        } else {
            stats->error_404++;
        }
    }
    if (sem) sem_post(sem);

    if (sent_bytes < 0) {
        serve_404(client_socket);
    }

    close(client_socket);
}