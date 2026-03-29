#include "common.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * client skeleton:
 * - connect to the server
 * - send join
 * - wait for weights
 * - compute a local gradient
 * - send gradient back
 * - keep going until terminate shows up
 */

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = DEFAULT_SERVER_PORT;

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = atoi(argv[2]);
    }

    printf("client skeleton connecting to %s:%d\n", host, port);
    printf("TODO: connect, join training session, and process server messages\n");

    return 0;
}
