#ifndef SERVER_H
#define SERVER_H

#include <sys/select.h>
#include <stdint.h>
#include "protocol.h"

#define MAX_CLIENTS_SUPPORTED (FD_SETSIZE - 1)  // -1 for the listening socket

/* Connection management and select() event loop. */

typedef struct {
    int socket_fd;           /* Client socket fd (-1 if unused) */
    int client_id;           /* Unique client identifier */
    int connected;           /* Is this client still connected? */
    ConnState recv_state;    /* Protocol state for incoming messages */
} ClientConnection;

typedef struct {
    int listening_socket;    /* Server's listening socket */
    ClientConnection clients[MAX_CLIENTS_SUPPORTED];  /* Client connections */
    int num_clients;         /* Current count of connected clients */
    fd_set read_set;         /* File descriptor set for select() */
    int max_fd;              /* Highest file descriptor number */
    int running;             /* Server running flag */
} ServerState;

/* Create TCP socket, set SO_REUSEADDR, bind, listen, set non-blocking.
 * Returns: socket fd on success, -1 on error. */
int server_initialize_listening_socket(int port);

/* Accept incoming connection, initialize ClientConnection.
 * Returns: new client socket fd on success, -1 on error or no connection ready. */
int server_accept_new_client(int listening_socket, ClientConnection *client);

/* Find available slot, assign client_id, add to fd_set, update max_fd.
 * Returns: client_id on success, -1 on failure (e.g., max clients exceeded). */
int server_add_client_to_set(ServerState *state, int client_fd);

/* Close socket, remove from fd_set, clean up ClientConnection, recalculate max_fd.
 * Returns: 0 on success, -1 if client not found. */
int server_remove_client_from_set(ServerState *state, int client_id);

/* Linear search to find client by ID.
 * Returns: pointer to ClientConnection on success, NULL if not found. */
ClientConnection *server_get_client_by_id(ServerState *state, int client_id);

/* Get the current number of connected clients.
 * Returns: number of connected clients, or -1 if state is NULL. */
int server_get_num_clients(ServerState *state);

/* Main event loop: call select() to monitor sockets, accept new connections, read messages.
 * Continues until state->running becomes false. */
void server_select_loop(ServerState *state);

/* Close all client and listening sockets, release resources. */
void server_cleanup(ServerState *state);

#endif /* SERVER_H */
