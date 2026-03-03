# --- Project Configuration ---
# The name of the final executable
TARGET = webserver

# Compiler and Flags
CC = gcc
# -Wall -Wextra: Show all warnings to ensure rigorous C basics
# -g: Include debug symbols for troubleshooting signals/forking
# -D_GNU_SOURCE: Enables certain modern features for shared memory/signals
CFLAGS = -Wall -Wextra -g -O2 -D_GNU_SOURCE

# --- Libraries (Requirement-Specific) ---
# -lrt: Required for POSIX Shared Memory (shm_open, shm_unlink) [cite: 44, 58]
# -lpthread: Required for POSIX Semaphores (sem_open, sem_wait) [cite: 49, 51]
LIBS = -lrt -lpthread

# --- File Structure ---
# List all .c files here. Based on your project needs:
# main: Master process loop and socket setup [cite: 32]
# http: Request parsing, file serving, and /status logic [cite: 38, 52]
# ipc: Shared memory and semaphore management 
# signals: SIGCHLD and SIGINT handling [cite: 56, 57]
SRCS = main.c http_handler.c ipc_manager.c signal_handlers.c

# Automatically generate a list of .o (object) files
OBJS = $(SRCS:.c=.o)

# Header files are listed so the project recompiles if a header changes
HEADERS = server_stats.h http_handler.h ipc_manager.h signal_handlers.h

# --- Build Rules ---

# Default target: build the webserver
all: $(TARGET)

# Linking the final executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LIBS)
	@echo "Build successful: ./$(TARGET)"

# Compiling individual source files into object files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# --- Utility Targets ---

# 'clean' removes compiled binaries and object files
clean:
	rm -f $(TARGET) $(OBJS)
	@echo "Cleaned build artifacts."

# 'distclean' also cleans up any persistent OS resources if the program crashed
# This is helpful because semaphores/shared memory can persist in /dev/shm [cite: 58]
distclean: clean
	@echo "Note: Ensure you manually check /dev/shm for leaked segments if server crashed."

# Rule to run the server (example on port 8080)
run: $(TARGET)
	./$(TARGET) 8080

.PHONY: all clean distclean run