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

void compute_gradient(float *weights, float *gradient, float *X, float y, int dim) {
    float prediction = 0.0f;
    for (int i = 0; i < dim; i++) {
        prediction += weights[i] * X[i];
    }

    float error = prediction - y;

    for (int i = 0; i < dim; i++) {
        gradient[i] = 2.0f * error * X[i];
    }
}

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
