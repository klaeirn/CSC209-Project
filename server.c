#include "common.h"
#include "protocol.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Main server program: initializes listening socket and runs select() event loop.
 */

float weights[DEFAULT_MODEL_DIM];
float gradient_buffer[THRESHOLD][DEFAULT_MODEL_DIM];
int gradient_count = 0;
int iterations = 0;
int should_terminate = 0;

ServerState server_state;

void broadcast_weights() {
    for (int i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
        if (server_state.clients[i].connected) {
            protocol_send_weights(server_state.clients[i].socket_fd, weights, DEFAULT_MODEL_DIM);
        }
    }
}

void broadcast_terminate() {
    for (int i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
        if (server_state.clients[i].connected) {
            protocol_send_terminate(server_state.clients[i].socket_fd);
        }
    }
}

void handle_message(const Message *msg) {
    if (msg == NULL) {
        return;
    }

    switch (msg->header.type) {
        case MSG_JOIN:
            printf("Client joined training\n");
            break;

        case MSG_GRADIENT:
            printf("Received gradient\n");
            handle_gradient(msg);
            break;

        case MSG_WEIGHTS:
            printf("Received unexpected WEIGHTS message from client\n");
            break;

        case MSG_TERMINATE:
            printf("Received TERMINATE from client\n");
            break;

        default:
            printf("Unknown message type: %u\n", msg->header.type);
            break;
    }
}

void handle_gradient(const Message *msg) {
    float gradient[DEFAULT_MODEL_DIM] = {0};
    uint32_t dim = 0;

    if (protocol_decode_gradient(msg, gradient, &dim) != 0) {
        fprintf(stderr, "Failed to decode gradient\n");
        return;
    }

    if (gradient_count >= THRESHOLD) {
        fprintf(stderr, "Gradient buffer full, dropping gradient\n");
        return;
    }

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

        iterations++;
        printf("Weights updated (iteration=%d)\n", iterations);

        broadcast_weights();

        if (iterations >= MAX_ITERATIONS) {
            printf("Reached MAX_ITERATIONS (%d), broadcasting TERMINATE\n", iterations);
            broadcast_terminate();
            should_terminate = 1;
        }

        gradient_count = 0;
    }
}

int main(int argc, char **argv) {
    int port = DEFAULT_SERVER_PORT;
    ServerState server_state;
    int listening_socket;

    if (argc > 1) {
        port = atoi(argv[1]);
    }

    printf("Server starting on port %d\n", port);

    /* Create listening socket */
    listening_socket = server_initialize_listening_socket(port);
    if (listening_socket < 0) {
        fprintf(stderr, "Failed to initialize listening socket\n");
        return 1;
    }

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