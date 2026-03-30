#include "common.h"
#include "protocol.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Main server program: initializes listening socket and runs select() event loop.
 */

int main(int argc, char **argv) {
    int port = DEFAULT_SERVER_PORT;
    ServerState server_state;
    int listening_socket;
    
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    printf("Server starting on port %d\n", port);

    /* Initialize server state */
    memset(&server_state, 0, sizeof(server_state));
    server_state.listening_socket = listening_socket;
    server_state.max_fd = listening_socket;
    server_state.running = 1;
    
    /* Add listening socket to the read_set */
    FD_ZERO(&server_state.read_set);
    FD_SET(listening_socket, &server_state.read_set);

    printf("Listening socket initialized and ready for connections\n");

    /* Run the main select() event loop */
    server_select_loop(&server_state);

    /* Clean up resources */
    server_cleanup(&server_state);

    printf("Server shutting down\n");
    return 0;
}