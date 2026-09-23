#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "http_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <errno.h>

#if defined(__linux__)
#include <sys/sendfile.h>
#endif


/**
 * @brief Parses an HTTP GET request and writes a full HTTP response into response_buf.
 */
int handle_http_request(const char *request_buf, size_t req_len, char *response_buf, size_t max_resp_len) {
    (void)req_len; // Unused for simple null-terminated/text parsing

    char method[16] = {0};
    char uri[256] = {0};
    char version[16] = {0};

    // 1. Basic HTTP request-line extraction
    if (sscanf(request_buf, "%15s %255s %15s", method, uri, version) < 2) {
        // Malformed Request -> 400 Bad Request
        return snprintf(response_buf, max_resp_len,
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 15\r\n"
            "Connection: close\r\n\r\n"
            "400 Bad Request");
    }

    if (strcmp(method, "GET") != 0) {
        // Method not allowed -> 405 Method Not Allowed
        return snprintf(response_buf, max_resp_len,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 22\r\n"
            "Connection: close\r\n\r\n"
            "405 Method Not Allowed");
    }

    // 2. Map route to local file
    char filepath[512] = {0};
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
        snprintf(filepath, sizeof(filepath), "index.html");
    } else {
        // Prevent directory traversal attacks (security check)
        if (strstr(uri, "..")) {
            return snprintf(response_buf, max_resp_len,
                "HTTP/1.1 403 Forbidden\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 13\r\n"
                "Connection: close\r\n\r\n"
                "403 Forbidden");
        }
        snprintf(filepath, sizeof(filepath), "public%s", uri);
    }

    // 3. Attempt to open and read the file
    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) {
        // 404 Not Found fallback
        const char *not_found_body = "<h1>404 Not Found</h1>";
        return snprintf(response_buf, max_resp_len,
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n"
            "%s", strlen(not_found_body), not_found_body);
    }

    // 4. Reserve header space and read file directly into response buffer body
    char header_temp[256];
    int body_offset = snprintf(header_temp, sizeof(header_temp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "Content-Length: ");

    // Read payload from disk
    char file_content[2048];
    ssize_t bytes_read = read(file_fd, file_content, sizeof(file_content));
    close(file_fd);

    if (bytes_read < 0) {
        return snprintf(response_buf, max_resp_len,
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 25\r\n"
            "Connection: close\r\n\r\n"
            "500 Internal Server Error");
    }

    // 5. Assemble final response
    int total_len = snprintf(response_buf, max_resp_len,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zd\r\n"
        "Connection: close\r\n\r\n"
        "%.*s",
        bytes_read, (int)bytes_read, file_content);

    return total_len;
}
/**
 * Determina el tipo MIME en función de la extensión del archivo.
 */
static const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".dat") == 0) return "application/octet-stream";
    return "text/plain";
}

/**
 * Envía un archivo al socket cliente usando sendfile (Zero-Copy).
 * Compatible con macOS (Darwin) y Linux.
 */
int serve_file_zero_copy(int client_socket, const char *file_path, const char *content_type) {
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        return -1;
    }

    off_t total_bytes = st.st_size;

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
        close(file_fd);
        return -1;
    }

#if defined(__APPLE__)
    // En macOS: sendfile(fd_archivo, fd_socket, offset_inicial, &longitud, cabeceras, flags)
    off_t len = total_bytes;
    if (sendfile(file_fd, client_socket, 0, &len, NULL, 0) < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            close(file_fd);
            return -1;
        }
    }
#elif defined(__linux__)
    // En Linux: sendfile(fd_socket, fd_archivo, &offset, conteo_bytes)
    off_t offset = 0;
    while (offset < total_bytes) {
        ssize_t bytes_sent = sendfile(client_socket, file_fd, &offset, total_bytes - offset);
        if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            close(file_fd);
            return -1;
        }
        if (bytes_sent == 0) break;
    }
#endif

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

/**
 * Función requerida por http_handler.h: sirve un archivo o responde 404.
 */
void serve_file(int client_socket, const char *path) {
    const char *mime = get_mime_type(path);
    if (serve_file_zero_copy(client_socket, path, mime) < 0) {
        serve_404(client_socket);
    }
}

/**
 * Punto de entrada para el proceso hijo al recibir una conexión.
 */
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