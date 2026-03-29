#include "common.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * server skeleton:
 * - set up the listening socket
 * - make it non-blocking
 * - accept clients as they show up
 * - use select() on the listening socket and client sockets
 * - pass readable sockets into protocol_read_message()
 * - hand full messages off to alice's training logic
 */

int main(int argc, char **argv) {
    int port = DEFAULT_SERVER_PORT;

    if (argc > 1) {
        port = atoi(argv[1]);
    }

    printf("server skeleton starting on port %d\n", port);
    printf("TODO: initialize listening socket, client table, and select() loop\n");

    return 0;
}
