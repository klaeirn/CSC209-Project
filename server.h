#ifndef SERVER_H
#define SERVER_H

#include <sys/select.h>
#include <stdint.h>
#include "protocol.h"

#define MAX_CLIENTS_SUPPORTED (FD_SETSIZE - 1)

typedef struct {
    int socket_fd;   
    int client_id; 
    int connected;   
    ConnState recv_state;  
} ClientConnection;

typedef struct {
    int listening_socket; 
    ClientConnection clients[MAX_CLIENTS_SUPPORTED];
    int num_clients;  
    fd_set read_set;      
    int max_fd;       
    int running;        
} ServerState;

void handle_gradient(const Message *msg);
int server_initialize_listening_socket(int port);

int server_accept_new_client(int listening_socket, ClientConnection *client);

int server_add_client_to_set(ServerState *state, int client_fd);

int server_remove_client_from_set(ServerState *state, int client_id);

ClientConnection *server_get_client_by_id(ServerState *state, int client_id);

int server_get_num_clients(ServerState *state);

void server_select_loop(ServerState *state);

void server_cleanup(ServerState *state);

#endif
