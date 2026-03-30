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

int client_fds[MAX_CLIENTS];
int num_clients;

float weights[DEFAULT_MODEL_DIM];
float gradient_buffer[THRESHOLD][DEFAULT_MODEL_DIM];
int gradient_count = 0;
int iterations = 0;

void broadcast_weights() {
    for (int i = 0; i < num_clients; i++) {
        protocol_send_weights(client_fds[i], weights, DEFAULT_MODEL_DIM);
    }
}

void broadcast_terminate() {
    for (int i = 0; i < num_clients; i++) {
        protocol_send_terminate(client_fds[i]);
    }
}

void handle_gradient(void *payload) {
    float *gradient = (float *)payload; 

    for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
        gradient_buffer[gradient_count][i] = gradient[i];
    }

    gradient_count++;

    if (gradient_count >= THRESHOLD) {
        float average[DEFAULT_MODEL_DIM] = {0};

        for (int j = 0; j < gradient_count; j++) {
            for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
                average[i] += gradient_buffer[j][i];
            }
        }
        
        for (int j = 0; j < DEFAULT_MODEL_DIM; j++) {
            average[j] = average[j] / gradient_count;
        }

        for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
            weights[i] -= LEARNING_RATE * average[i];
        }

        printf("Weights updated\n");

        broadcast_weights();

        iterations++;
        gradient_count = 0;
    }
}

int main(int argc, char **argv) {
    int port = DEFAULT_SERVER_PORT;

    if (argc > 1) {
        port = atoi(argv[1]);
    }

    printf("server skeleton starting on port %d\n", port);

    for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
        weights[i] = 0.0f;
    }

    return 0;
}
