#include "server.h"

#include <stdio.h>
#include <stdlib.h>

extern int should_terminate;
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Helper: Set socket to non-blocking mode. */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int server_initialize_listening_socket(int port) {
    /* Create TCP socket, set SO_REUSEADDR, bind, listen, and set non-blocking. */
    int socket_fd;
    struct sockaddr_in addr;
    int reuse = 1;

    /* Create TCP socket */
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        return -1;
    }

    /* Allow reusing the port immediately after shutdown */
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(socket_fd);
        return -1;
    }

    /* Bind to localhost:port */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(socket_fd);
        return -1;
    }

    /* Mark socket as ready to accept connections */
    if (listen(socket_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(socket_fd);
        return -1;
    }

    /* Set to non-blocking for select() loop */
    if (set_nonblocking(socket_fd) < 0) {
        perror("fcntl");
        close(socket_fd);
        return -1;
    }

    fprintf(stderr, "Listening socket initialized successfully on port %d\n", port);
    return socket_fd;
}

int server_accept_new_client(int listening_socket, ClientConnection *client) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd;
    
    /* Accept new connection */
    client_fd = accept(listening_socket, (struct sockaddr *)&client_addr, &addr_len);

    /* Handle non-blocking: EAGAIN/EWOULDBLOCK means no connection ready yet */
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        perror("accept");
        return -1;
    }

    /* Set to non-blocking */
    if (set_nonblocking(client_fd) < 0) {
        perror("fcntl");
        close(client_fd);
        return -1;
    }

    /* Initialize ClientConnection */
    client->socket_fd = client_fd;
    client->connected = 1;
    client->client_id = -1;
    protocol_init_conn_state(&client->recv_state);

    fprintf(stderr, "New client accepted: %s:%d (fd=%d)\n",
            inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), client_fd);

    return client_fd;
}

int server_add_client_to_set(ServerState *state, int client_fd) {
    /* Find available slot, assign client_id, add to read_set, update max_fd. */
    int i;
    int assigned_id;

    if (state == NULL || client_fd < 0) {
        return -1;
    }

    /* Find available slot */
    assigned_id = -1;
    for (i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
        if (state->clients[i].connected == 0) {
            assigned_id = i;
            break;
        }
    }

    if (assigned_id < 0) {
        fprintf(stderr, "No available slots for new client (max clients exceeded)\n");
        return -1;
    }

    /* Use array index as client_id and initialize ClientConnection */
    state->clients[assigned_id].socket_fd = client_fd;
    state->clients[assigned_id].connected = 1;
    state->clients[assigned_id].client_id = assigned_id;
    protocol_init_conn_state(&state->clients[assigned_id].recv_state);

    /* Add to fd_set and update max_fd */
    FD_SET(client_fd, &state->read_set);
    if (client_fd > state->max_fd) {
        state->max_fd = client_fd;
    }

    state->num_clients++;
    fprintf(stderr, "Client added to set: client_id=%d, socket_fd=%d, total_clients=%d\n",
            assigned_id, client_fd, state->num_clients);

    return assigned_id;
}

int server_remove_client_from_set(ServerState *state, int client_id) {
    /* Close socket, remove from fd_set, clean up ClientConnection, recalculate max_fd. */
    ClientConnection *client;
    int old_fd;
    int i, new_max_fd;

    if (state == NULL || client_id < 0 || client_id >= MAX_CLIENTS_SUPPORTED) {
        return -1;
    }

    /* Find the client by ID */
    client = &state->clients[client_id];
    if (client->connected == 0 || client->client_id != client_id) {
        fprintf(stderr, "Client not found: client_id=%d\n", client_id);
        return -1;
    }

    old_fd = client->socket_fd;

    /* Close socket and remove from fd_set */
    if (close(client->socket_fd) < 0) {
        perror("close");
    }
    FD_CLR(old_fd, &state->read_set);

    /* Clean up ClientConnection */
    protocol_reset_conn_state(&client->recv_state);
    client->connected = 0;
    client->socket_fd = -1;
    client->client_id = -1;

    state->num_clients--;

    /* Recalculate max_fd if the removed fd was the highest */
    if (old_fd == state->max_fd) {
        new_max_fd = state->listening_socket;
        for (i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
            if (state->clients[i].connected && state->clients[i].socket_fd > new_max_fd) {
                new_max_fd = state->clients[i].socket_fd;
            }
        }
        state->max_fd = new_max_fd;
    }

    fprintf(stderr, "Client removed: client_id=%d, socket_fd=%d, total_clients=%d\n",
            client_id, old_fd, state->num_clients);

    return 0;
}

ClientConnection *server_get_client_by_id(ServerState *state, int client_id) {
    /* Linear search to find client by ID. */
    int i;

    if (state == NULL || client_id < 0 || client_id >= MAX_CLIENTS_SUPPORTED) {
        return NULL;
    }

    for (i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
        if (state->clients[i].connected && state->clients[i].client_id == client_id) {
            return &state->clients[i];
        }
    }

    return NULL;
}

int server_get_num_clients(ServerState *state) {
    /* Get the current number of connected clients. */
    if (state == NULL) {
        return -1;
    }
    return state->num_clients;
}

void server_select_loop(ServerState *state) {
    /* Main event loop: accept new clients, read messages, detect disconnects. */
    fd_set current_set;
    struct timeval tv;
    int select_ret;
    int i;
    ClientConnection *client;
    Message incoming_msg;
    int read_result;

    if (!state->running) {
        state->running = 1;
    }

    while (state->running) {
        /* Prepare for select() with 1-second timeout */
        memcpy(&current_set, &state->read_set, sizeof(fd_set));
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        select_ret = select(state->max_fd + 1, &current_set, NULL, NULL, &tv);

        if (select_ret < 0) {
            if (errno == EINTR) {
                fprintf(stderr, "select() interrupted by signal\n");
                continue;
            }
            perror("select");
            break;
        }

        if (select_ret == 0) {
            /* Timeout, continue */
            continue;
        }

        /* Check if listening socket has new connections */
        if (FD_ISSET(state->listening_socket, &current_set)) {
            ClientConnection new_client;
            int client_fd = server_accept_new_client(state->listening_socket, &new_client);

            if (client_fd >= 0) {
                int client_id = server_add_client_to_set(state, client_fd);
                if (client_id < 0) {
                    close(client_fd);
                }
            }
        }

        /* Check whether server already decided to terminate training */
        if (should_terminate) {
            break;
        }

        /* Check each connected client for incoming data */
        for (i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
            client = &state->clients[i];

            if (!client->connected || client->socket_fd < 0) {
                continue;
            }

            if (FD_ISSET(client->socket_fd, &current_set)) {
                read_result = protocol_read_message(client->socket_fd, &client->recv_state, &incoming_msg);

                if (read_result == 1) {
                    /* Complete message received - dispatch to handler */
                    fprintf(stderr, "Message received from client_id=%d, type=%u, length=%u\n",
                            client->client_id, incoming_msg.header.type, incoming_msg.header.length);

                    /* Call the message handler (defined in training.c/server.c) */
                    extern void handle_message(const Message *msg);
                    handle_message(&incoming_msg);

                    protocol_free_message(&incoming_msg);
                } else if (read_result == -1) {
                    /* Error or disconnect */
                    fprintf(stderr, "Client disconnect or error: client_id=%d\n", client->client_id);
                    server_remove_client_from_set(state, client->client_id);
                }
                /* read_result == 0 means partial message, do nothing */
            }
        }
    }
}

void server_cleanup(ServerState *state) {
    /* Close all client and listening sockets, release resources. */
    int i;

    if (state == NULL) {
        return;
    }

    /* Remove all connected clients */
    for (i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
        if (state->clients[i].connected) {
            server_remove_client_from_set(state, state->clients[i].client_id);
        }
    }

    /* Close listening socket */
    if (state->listening_socket >= 0) {
        if (close(state->listening_socket) < 0) {
            perror("close listening socket");
        } else {
            fprintf(stderr, "Listening socket closed\n");
        }
    }

    /* Reset server state */
    state->listening_socket = -1;
    state->num_clients = 0;
    state->running = 0;
    FD_ZERO(&state->read_set);
}