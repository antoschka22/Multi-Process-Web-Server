TARGET = webserver

# Default compiler (Clang on macOS, GCC on Linux)
CC ?= gcc

# Base warning and debug flags
# -fno-omit-frame-pointer ensures accurate stack traces for Flame Graphs
CFLAGS = -Wall -Wextra -g -O2 -fno-omit-frame-pointer -D_GNU_SOURCE

# Platform detection for OS-specific libraries/flags
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    # Linux requires -lrt and -pthread for POSIX shared memory and semaphores
    LIBS = -lrt -pthread
else ifeq ($(UNAME_S),Darwin)
    # macOS built-in libraries handle shm/sem natively
    LIBS = -pthread
endif

# --- File Structure ---
SRCS = main.c http_handler.c ipc_manager.c signal_handlers.c
OBJS = $(SRCS:.c=.o)
HEADERS = server_stats.h http_handler.h ipc_manager.h signal_handlers.h

# --- Build Rules ---
all: $(TARGET)

# Linking the final executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LIBS)
	@echo "Build successful: ./$(TARGET)"

# Compiling individual source files into object files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# --- Utility Targets ---
clean:
	rm -f $(TARGET) $(OBJS) *.svg *.txt
	@echo "Cleaned build artifacts and profiling logs."

distclean: clean
	@echo "Note: Check /dev/shm (Linux) or ipcs (macOS) for leaked shared memory segments."

run: $(TARGET)
	./$(TARGET) 8080

.PHONY: all clean distclean run