#include "server.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern int should_terminate;
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int server_initialize_listening_socket(int port) {
    int socket_fd;
    struct sockaddr_in addr;
    int reuse = 1;

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(socket_fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(socket_fd);
        return -1;
    }

    if (listen(socket_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(socket_fd);
        return -1;
    }

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
    
    client_fd = accept(listening_socket, (struct sockaddr *)&client_addr, &addr_len);

    if (client_fd < 0) {
        perror("accept");
        return -1;
    }

    if (set_nonblocking(client_fd) < 0) {
        perror("fcntl");
        close(client_fd);
        return -1;
    }

    client->socket_fd = client_fd;
    client->connected = 1;
    client->client_id = -1;
    protocol_init_conn_state(&client->recv_state);

    fprintf(stderr, "New client accepted: %s:%d (fd=%d)\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), client_fd);

    return client_fd;
}

int server_add_client_to_set(ServerState *state, int client_fd) {
    int i;
    int assigned_id;

    if (state == NULL || client_fd < 0) {
        return -1;
    }

    // find an available slot
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

    // add the client to the set
    state->clients[assigned_id].socket_fd = client_fd;
    state->clients[assigned_id].connected = 1;
    state->clients[assigned_id].client_id = assigned_id;
    protocol_init_conn_state(&state->clients[assigned_id].recv_state);
    memset(&state->clients[assigned_id].outgoing_msg, 0, sizeof(state->clients[assigned_id].outgoing_msg));
    state->clients[assigned_id].has_outgoing = 0;

    // add the socket to the read set
    FD_SET(client_fd, &state->read_set);
    // update the max fd if the new client has a higher fd
    if (client_fd > state->max_fd) {
        state->max_fd = client_fd;
    }

    state->num_clients++;
    fprintf(stderr, "Client added to set: client_id=%d, socket_fd=%d, total_clients=%d\n", assigned_id, client_fd, state->num_clients);

    return assigned_id;
}

int server_remove_client_from_set(ServerState *state, int client_id) {
    // get the client from the set
    ClientConnection *client;
    int old_fd;
    int i, new_max_fd;

    if (state == NULL || client_id < 0 || client_id >= MAX_CLIENTS_SUPPORTED) {
        return -1;
    }

    client = &state->clients[client_id];
    if (client->connected == 0 || client->client_id != client_id) {
        fprintf(stderr, "Client not found: client_id=%d\n", client_id);
        return -1;
    }

    old_fd = client->socket_fd;

    if (close(client->socket_fd) < 0) {
        perror("close");
    }
    FD_CLR(old_fd, &state->read_set);

    if (client->has_outgoing) {
        protocol_free_outgoing(&client->outgoing_msg);
        client->has_outgoing = 0;
    }
    // reset the connection state
    protocol_reset_conn_state(&client->recv_state);
    client->connected = 0;
    client->socket_fd = -1;
    client->client_id = -1;

    // update the number of clients
    state->num_clients--;
    if (state->num_clients == 0) {
        handle_no_clients_remaining();
    }

    // update the max fd if the client bing removed has the max fd
    if (old_fd == state->max_fd) {
        new_max_fd = state->listening_socket;
        for (i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
            if (state->clients[i].connected && state->clients[i].socket_fd > new_max_fd) {
                new_max_fd = state->clients[i].socket_fd;
            }
        }
        state->max_fd = new_max_fd;
    }

    fprintf(stderr, "Client removed: client_id=%d, socket_fd=%d, total_clients=%d\n",client_id, old_fd, state->num_clients);

    return 0;
}

ClientConnection *server_get_client_by_id(ServerState *state, int client_id) {
    int i;

    if (state == NULL || client_id < 0 || client_id >= MAX_CLIENTS_SUPPORTED) {
        return NULL;
    }

    // find the client by id
    for (i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
        if (state->clients[i].connected && state->clients[i].client_id == client_id) {
            return &state->clients[i];
        }
    }

    return NULL;
}

int server_get_num_clients(ServerState *state) {
    // return the number of clients
    if (state == NULL) {
        return -1;
    }
    return state->num_clients;
}

void server_select_loop(ServerState *state) {
    // set up the read and write sets
    fd_set curr_read_set;
    fd_set curr_write_set;
    struct timeval tv;
    int select_ret;
    int i;
    ClientConnection *client;
    // incomin message
    Message incoming_msg;
    int read_result;

    if (!state->running) {
        state->running = 1;
    }

    while (state->running) {
        // copy the read set
        memcpy(&curr_read_set, &state->read_set, sizeof(fd_set));
        // zero th write set
        FD_ZERO(&curr_write_set);
        for (i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
            // get the client from the set
            client = &state->clients[i];
            // add to the wrte set f the client is cnnected and has outgoing messages
            if (client->connected && client->socket_fd >= 0 && client->has_outgoing) {
                FD_SET(client->socket_fd, &curr_write_set);
            }
        }
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        select_ret = select(state->max_fd + 1, &curr_read_set, &curr_write_set, NULL, &tv);

        if (select_ret < 0) {
            if (errno == EINTR) {
                fprintf(stderr, "select() interrupted by signal\n");
                continue;
            }
            perror("select");
            break;
        }

        if (select_ret == 0) {
            handle_server_tick();
            continue;
        }

        if (FD_ISSET(state->listening_socket, &curr_read_set)) {
            // accept a new client
            ClientConnection new_client;
            int client_fd = server_accept_new_client(state->listening_socket, &new_client);

            if (client_fd >= 0) {
                // add the client to the set
                int client_id = server_add_client_to_set(state, client_fd);
                if (client_id < 0) {
                    close(client_fd);
                }
            }
        }

        if (should_terminate) {
            break;
        }

        for (i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
            // check if the server should terminate
            if (should_terminate) {
                state->running = 0;
                break;
            }

            // get the client from the set
            client = &state->clients[i];
            if (!client->connected || client->socket_fd < 0) {
                continue;
            }

            if (FD_ISSET(client->socket_fd, &curr_read_set)) {
                // read the message from the client
                read_result = protocol_read_message(client->socket_fd, &client->recv_state, &incoming_msg);

                if (read_result == 1) {
                    // message received
                    fprintf(stderr, "Message received from client_id=%d, type=%u, length=%u\n", client->client_id, incoming_msg.header.type, incoming_msg.header.length);

                    // handle the message
                    extern void handle_message(const Message *msg);
                    handle_message(&incoming_msg);

                    // free the message
                    protocol_free_message(&incoming_msg);
                    // check should terminate
                    if (should_terminate) {
                        state->running = 0;
                        break;
                    }
                } else if (read_result == -1) {
                    // client disconnected or error
                    fprintf(stderr, "Client disconnect or error: client_id=%d\n", client->client_id);
                    server_remove_client_from_set(state, client->client_id);
                    continue;
                }
            }

            if (!client->connected || client->socket_fd < 0 || !client->has_outgoing) {
                continue;
            }

            if (FD_ISSET(client->socket_fd, &curr_write_set)) {
                // flush the outgoing message
                int flush_result = protocol_flush_outgoing(client->socket_fd, &client->outgoing_msg);
                if (flush_result == 1) {
                    // message sent
                    protocol_free_outgoing(&client->outgoing_msg);
                    client->has_outgoing = 0;
                } else if (flush_result < 0) {
                    // client write error
                    fprintf(stderr, "Client write error: client_id=%d\n", client->client_id);
                    server_remove_client_from_set(state, client->client_id);
                }
            }
        }

        handle_server_tick();
    }
}

void server_cleanup(ServerState *state) {
    int i;

    // remove all clients from the set
    for (i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
        if (state->clients[i].connected) {
            server_remove_client_from_set(state, state->clients[i].client_id);
        }
    }

    // close the listening socket
    if (state->listening_socket >= 0) {
        if (close(state->listening_socket) < 0) {
            perror("close listening socket");
        } else {
            fprintf(stderr, "Listening socket closed\n");
        }
    }
    
    state->listening_socket = -1;
    state->num_clients = 0;
    state->running = 0;
    FD_ZERO(&state->read_set);
}