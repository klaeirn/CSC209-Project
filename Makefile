CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lm
PORT ?= 4242

# source files
SERVER_SRCS = server.c connection.c protocol.c
CLIENT_SRCS = client.c protocol.c

# object files
SERVER_OBJS = $(SERVER_SRCS:.c=.o)
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

# build targets
SERVER_BINARY = server
CLIENT_BINARY = client
BINARIES = $(SERVER_BINARY) $(CLIENT_BINARY)

# default target
all: $(BINARIES)

# build the server
$(SERVER_BINARY): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# build the client
$(CLIENT_BINARY): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# compile source files into object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# clean object files and binaries
clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS) $(BINARIES)

# clean only object files and keep binaries
clean-obj:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS)

# rebuild from scratch
rebuild: clean all

# run the server on configured port
run-server: $(SERVER_BINARY)
	./$(SERVER_BINARY) $(PORT)

# run one client on configured port
run-client: $(CLIENT_BINARY)
	./$(CLIENT_BINARY) 127.0.0.1 $(PORT)

# run server with tiny stress test mode
run-stress: $(SERVER_BINARY)
	./$(SERVER_BINARY) $(PORT) 12 1

# phony targets
.PHONY: all clean clean-obj rebuild run-server run-client run-stress
