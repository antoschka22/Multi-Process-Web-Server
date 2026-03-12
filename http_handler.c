/**
 * @file http_handler.c
 * @brief HTTP request parser, file server, and metric dashboard generator.
 *
 * This module runs entirely within the child processes. It handles incoming
 * network streams, parses HTTP methods, serves local files, and dynamically
 * generates the live "/status" dashboard. It uses POSIX semaphores to safely
 * update global tracking metrics without race conditions.
 */

#include "http_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

void handle_client(int client_fd, server_metrics_t* stats, sem_t* sem) {
    char buffer[1024];
    
    // Read the incoming data stream from the client.
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) return;
    buffer[bytes_read] = '\0';

    char method[10], path[256];
    
    // Parse the HTTP GET request to determine the requested file path.
    sscanf(buffer, "%s %s", method, path);

    // Acquire lock before updating shared memory counters to prevent data corruption.
    sem_wait(sem);
    stats->total_requests++;
    // Safely increment memory counters then release the semaphore for waiting processes.
    sem_post(sem);

    /* Built-in Live Dashboard Endpoint */
    // If the client asks for "/status", bypass the file system entirely.
    if (strcmp(path, "/status") == 0) {
        char response[1024];
        
        // Securely read the current values from the shared memory block.
        sem_wait(sem);
        // Dynamically generate an HTML page displaying live traffic and health stats.
        int len = snprintf(response, sizeof(response), 
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                    "<h1>Server Status</h1>"
                    "<p>Total Requests: %llu</p>"
                    "<p>200 OK: %llu</p>"
                    "<p>404 Not Found: %llu</p>",
                    stats->total_requests, stats->success_200, stats->error_404);
        sem_post(sem);
        
        write(client_fd, response, len);
    } else {
        /* Standard Static File Serving */
        // Attempt to locate the requested file on the server's local file system.
        char *file_path = path + 1; // Remove leading '/'
        int fd = open(file_path, O_RDONLY);
        
        if (fd != -1) {
            struct stat st;
            fstat(fd, &st);
            
            // If the file exists, generate a standard HTTP 200 OK response header.
            write(client_fd, "HTTP/1.1 200 OK\r\n\r\n", 19);
            
            char file_buf[1024];
            ssize_t n;
            // Send the file's contents over the network.
            while ((n = read(fd, file_buf, sizeof(file_buf))) > 0) {
                write(client_fd, file_buf, n);
            }
            close(fd);

            // Update success tracking metrics using the semaphore lock.
            sem_wait(sem);
            stats->success_200++;
            sem_post(sem);
        } else {
            // If the file is missing, respond with an HTTP 404 Not Found error.
            const char *not_found = "HTTP/1.1 404 Not Found\r\n\r\nFile Not Found";
            write(client_fd, not_found, strlen(not_found));
            
            // Update error tracking metrics securely.
            sem_wait(sem);
            stats->error_404++;
            sem_post(sem);
        }
    }
    
    // Once the response is completely sent, close the connection.
    close(client_fd);
    
    // Terminate the child process.
    exit(0); 
}