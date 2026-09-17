#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <netinet/tcp.h>
#include <errno.h>

/**
 * Serves a static file to the client socket using zero-copy sendfile(2)
 *
 * @param client_socket Connected client file descriptor
 * @param file_path     Path to the requested static file
 * @param content_type  MIME type (e.g., "text/html", "image/png")
 * @return 0 on success, -1 on failure
 */
int serve_file_zero_copy(int client_socket, const char *file_path, const char *content_type) {
    // Open the requested file in read-only mode
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        perror("open failed");
        return -1;
    }

    // Fetch metadata to determine exact byte length for Content-Length
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        perror("fstat failed");
        close(file_fd);
        return -1;
    }

    off_t total_bytes = st.st_size;

    // Format and send HTTP headers
    char header_buffer[512];
    int header_len = snprintf(header_buffer, sizeof(header_buffer),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "Server: C-ZeroCopy-Server/1.0\r\n"
        "\r\n",
        content_type, (long)total_bytes);

    if (send(client_socket, header_buffer, header_len, 0) < 0) {
        perror("send header failed");
        close(file_fd);
        return -1;
    }

    // Enable TCP_CORK to coalesce packets
    int cork = 1;
    setsockopt(client_socket, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));

    // Stream data directly in kernel space via sendfile loop
    off_t offset = 0;
    while (offset < total_bytes) {
        // sendfile(out_fd, in_fd, offset_ptr, count)
        // offset is updated automatically by the kernel
        ssize_t bytes_sent = sendfile(client_socket, file_fd, &offset, total_bytes - offset);

        if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                // Interrupted or socket buffer temporarily full
                continue;
            }
            perror("sendfile failed");
            close(file_fd);
            return -1;
        }

        if (bytes_sent == 0) {
            // EOF reached unexpectedly
            break;
        }
    }

    // Uncork socket to flush remaining data
    cork = 0;
    setsockopt(client_socket, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));

    close(file_fd);
    return 0;
}