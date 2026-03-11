#include "http_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

void handle_client(int client_fd, server_metrics_t* stats, sem_t* sem) {
    char buffer[1024];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) return;
    buffer[bytes_read] = '\0';

    char method[10], path[256];
    sscanf(buffer, "%s %s", method, path);

    // Update total requests
    sem_wait(sem);
    stats->total_requests++;
    sem_post(sem);

    if (strcmp(path, "/status") == 0) {
        char response[1024]; // Increased size for safety
        sem_wait(sem);
        // Inside handle_client() for the "/status" path:
int len = snprintf(response, sizeof(response), 
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
            "<h1>Server Status</h1>"
            "<p>Total Requests: %llu</p>"
            "<p>200 OK: %llu</p>"
            "<p>404 Not Found: %llu</p>"
            "<p>Total Bytes Sent: %llu</p>", // Add this line
            stats->total_requests, stats->success_200, stats->error_404, stats->total_bytes_sent); // Add stats->total_bytes_sent
        sem_post(sem);
        write(client_fd, response, len);
    } else {
        // Serve local file [cite: 72]
        // Remove leading '/' for local path
        char *file_path = path + 1;
        int fd = open(file_path, O_RDONLY);
        
        if (fd != -1) {
            struct stat st;
            fstat(fd, &st);
            write(client_fd, "HTTP/1.1 200 OK\r\n\r\n", 19);
            
            // Inside handle_client() when serving a local file:
            char file_buf[1024];
            ssize_t n;
            size_t bytes_sent_for_file = 19; // Start with the 19 bytes of the "HTTP/1.1 200 OK\r\n\r\n" header

            while ((n = read(fd, file_buf, sizeof(file_buf))) > 0) {
                write(client_fd, file_buf, n);
                bytes_sent_for_file += n; // Accumulate bytes
            }
            close(fd);

            sem_wait(sem);
            stats->success_200++;
            stats->total_bytes_sent += bytes_sent_for_file; // Update the shared memory counter safely
            sem_post(sem);
        } else {
            // 404 Not Found [cite: 74]
            const char *not_found = "HTTP/1.1 404 Not Found\r\n\r\nFile Not Found";
            write(client_fd, not_found, strlen(not_found));
            
            sem_wait(sem);
            stats->error_404++;
            sem_post(sem);
        }
    }
    close(client_fd);
    exit(0); // Terminate child process [cite: 75]
}