CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lm

# Source files
SERVER_SRCS = server.c connection.c protocol.c
CLIENT_SRCS = client.c protocol.c

# Object files
SERVER_OBJS = $(SERVER_SRCS:.c=.o)
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

# Targets
SERVER_BINARY = server
CLIENT_BINARY = client
BINARIES = $(SERVER_BINARY) $(CLIENT_BINARY)

# Default target
all: $(BINARIES)

# Build server
$(SERVER_BINARY): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Build client
$(CLIENT_BINARY): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files to object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean object files and binaries
clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS) $(BINARIES)

# Clean only object files (keep binaries)
clean-obj:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS)

# Rebuild from scratch
rebuild: clean all

# Phony targets
.PHONY: all clean clean-obj rebuild
