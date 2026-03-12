# Metric-Aware Multi-Process Web Server

## Overview
The "Metric-Aware" Multi-Process Web Server is a comprehensive system programming project that combines network communication, concurrent process management, and safe inter-process data sharing into a single application. At its core, it is a functional HTTP web server built entirely from scratch using standard C library socket APIs, bypassing high-level web frameworks. 

The server is designed to handle concurrent client connections by utilizing the `fork()` system call. While the parent process continuously listens for new connections, independent child processes take full responsibility for parsing HTTP GET requests and serving local files (or returning 404 errors). 

A standout feature of this project is its live statistics tracking. All isolated child processes securely update global counters (such as total requests, 200 OKs, and 404 errors) in a shared memory segment. This data is safely synchronized using POSIX semaphores and can be viewed dynamically by accessing the `/status` endpoint.

## Motivation
I developed this project to solidify and refine the practical skills I acquired during my **Operating Systems course at TU Wien**. It serves as a hands-on application of low-level system concepts, specifically focusing on:
* **Inter-Process Communication (IPC):** Designing safe data exchange mechanisms between isolated processes.
* **Shared Memory & Synchronization:** Using `mmap`/`shm_open` and POSIX semaphores to prevent race conditions during concurrent data access.
* **Process Management:** Utilizing `fork()` for concurrency and `waitpid` to prevent resource-draining "zombie" processes.
* **Signal Handling:** Implementing asynchronous signal handlers for graceful shutdowns (e.g., intercepting `SIGINT` via Ctrl+C) to safely unlink and destroy IPC resources without memory leaks.
* **Networking & Sockets:** Managing TCP connections, binding ports, and raw HTTP request/response parsing.
* **Error Handling & Argument Parsing:** Writing robust C code that anticipates and safely manages system call failures and user inputs.

## Features
* **Concurrent Request Handling:** Spawns a dedicated child process for every incoming client connection.
* **Static File Serving:** Reads and transmits local files, returning standard `HTTP 200 OK` or `HTTP 404 Not Found` responses.
* **Live Dashboard (`/status`):** Bypasses the file system to serve a dynamically generated HTML/JSON dashboard reflecting real-time server traffic and health.
* **Thread-Safe Metrics:** Employs POSIX semaphores as locks so multiple child processes can safely increment shared counters simultaneously.
* **Clean Resource Management:** Catches `SIGCHLD` to reap finished child processes and intercepts keyboard interrupts to destroy shared memory blocks and semaphores before exiting.

## Technologies Used
* **Language:** C
* **APIs:** POSIX (Sockets, Shared Memory, Semaphores, Signals)
* **Build System:** Make / GCC

## Building and Running

1. **Compile the project:**
   ```bash
   make
   ```
2. **Run the server** (defaults to port 8080, or specify a port as an argument)
    ```bash
    ./webserver 8080
    ```
3. **Clean build artifacts**
    ```bash
    make clean
    ```
4. **Deep clean** (helpful if the server crashes and leaces persistent OS resources in `/dev/shm`):
    ```bash
    make distclean
    ```

## Testing the Server
Once the server is running on port 8080, you can test its functionality using your web browser or via command-line tools like `curl``.
1. **Serving a valid HTML file**
Ensure you have an `index.html` file in your project directory.
* **Browser:** Navigate to `http://localhost:8080/index.html`
* **cURL**: 
    ```bash
    curl -i http://localhost:8080/index.html
    ```
*(This should return an HTTP 200 OK along with the file contents).*
2. **Triggering a 404 Not Found error:**
Request a file that doesnt exist on the server to test the error handling.
* **Browser:** Navigate to `http://localhost:8080/doesnotexist.html``
* **cURL:**
    ```bash
    curl -i http://localhost:8080/doesnotexist.html
    ```
*(This should return an HTTP 404 Not Found response).*

3. **Viewing the Live IPC Status Dashboard:**
To test the shared memory and semaphore functionality, check the live server metrics.
* **Browser:** Navigate to `http://localhost:8080/status`
* **cURL:**
    ```bash
    curl -i http://localhost:8080/status
    ```
*(This dynamically generates an HTML response showing the current number of Total Requests, 200 OKs, and 404 Errors). Tip: Try requesting a mix of valid files, invalid files, and the status page multiple times to watch the shared memory counters increment perfectly without race conditions!*