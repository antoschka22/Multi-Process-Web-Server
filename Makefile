TARGET = webserver

# Default compiler
CC ?= gcc

# Base warning, optimization, and debugging flags
# -fno-omit-frame-pointer ensures accurate stack traces for perf/Flame Graphs
CFLAGS = -Wall -Wextra -O2 -g -fno-omit-frame-pointer

# Platform detection
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    # io_uring requires -luring; POSIX shm/sem require -lrt and -pthread
    LIBS = -luring -lrt -pthread
else ifeq ($(UNAME_S),Darwin)
    $(error io_uring is a Linux-only subsystem. Please build and run inside Docker/Linux VM)
endif

# --- File Structure ---
SRCS = main.c http_handler.c ipc_manager.c signal_handlers.c
OBJS = $(SRCS:.c=.o)
HEADERS = uring_server.h server_stats.h http_handler.h ipc_manager.h signal_handlers.h

# --- Build Rules ---
all: $(TARGET)

# Linking the final executable (ensure $(LIBS) comes AFTER $(OBJS))
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
	@echo "Note: Check /dev/shm for leaked shared memory segments."

run: $(TARGET)
	./$(TARGET) 8080

.PHONY: all clean distclean run