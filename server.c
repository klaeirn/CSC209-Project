#include "common.h"
#include "protocol.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>

float weights[DEFAULT_MODEL_DIM];
float gradient_buffer[THRESHOLD][DEFAULT_MODEL_DIM];
int gradient_count = 0;
int iterations = 0;
int should_terminate = 0;

ServerState server_state;
static pid_t spawned_client_pids[64];
static int spawned_client_count = 0;

void handle_gradient(const Message *msg);

static int parse_int_arg(const char *text, int min_value, int max_value, int *out_value) {
    char *endptr;
    long parsed;

    if (text == NULL || out_value == NULL) {
        return -1;
    }

    errno = 0;
    parsed = strtol(text, &endptr, 10);
    if (errno != 0 || endptr == text || *endptr != '\0') {
        return -1;
    }
    if (parsed < min_value || parsed > max_value) {
        return -1;
    }

    *out_value = (int)parsed;
    return 0;
}

static void spawn_local_clients(const char *host, int port, int count) {
    char port_str[16];

    if (count <= 0) {
        return;
    }

    snprintf(port_str, sizeof(port_str), "%d", port);

    for (int i = 1; i <= count && spawned_client_count < (int)(sizeof(spawned_client_pids) / sizeof(spawned_client_pids[0])); i++) {
        char shard_path[64];
        pid_t pid;

        snprintf(shard_path, sizeof(shard_path), "data/client_%d.csv", i);
        pid = fork();
        if (pid < 0) {
            perror("fork");
            continue;
        }

        if (pid == 0) {
            execl("./client",
                  "./client",
                  host,
                  port_str,
                  shard_path,
                  (char *)NULL);
            perror("execl ./client");
            _exit(1);
        }

        spawned_client_pids[spawned_client_count++] = pid;
    }
}

static void wait_for_spawned_clients(void) {
    for (int i = 0; i < spawned_client_count; i++) {
        int status = 0;
        while (waitpid(spawned_client_pids[i], &status, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
    }
}

void broadcast_weights() {
    printf("Broadcasting updated weights to all clients...\n");
    for (int i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
        if (server_state.clients[i].connected) {
            if (protocol_send_weights(server_state.clients[i].socket_fd,
                                      weights,
                                      DEFAULT_MODEL_DIM) != 0) {
                fprintf(stderr, "Failed to send weights to client_id=%d (fd=%d)\n",
                        server_state.clients[i].client_id,
                        server_state.clients[i].socket_fd);
            }
        }
    }
    printf("Weights broadcast complete.\n");
}


void broadcast_terminate() {
    printf("Broadcasting TERMINATE to all clients...\n");
    for (int i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
        if (server_state.clients[i].connected) {
            if (protocol_send_terminate(server_state.clients[i].socket_fd) != 0) {
                fprintf(stderr, "Failed to send terminate to client_id=%d (fd=%d)\n",
                        server_state.clients[i].client_id,
                        server_state.clients[i].socket_fd);
            }
        }
    }
    printf("Terminate broadcast complete. Training finished.\n");
}

void handle_gradient(const Message *msg) {
    float gradient[DEFAULT_MODEL_DIM] = {0};
    uint32_t dim = 0;

    if (protocol_decode_gradient(msg, gradient, &dim) != 0) {
        fprintf(stderr, "Failed to decode gradient\n");
        return;
    }
    if (dim != DEFAULT_MODEL_DIM) {
        fprintf(stderr, "Gradient dimension mismatch: expected=%d got=%u\n",
                DEFAULT_MODEL_DIM,
                dim);
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
    printf("Gradient %d/%d received\n", gradient_count, THRESHOLD);

    if (gradient_count >= THRESHOLD) {
        printf("Aggregating gradients from %d clients...\n", gradient_count);
        float average[DEFAULT_MODEL_DIM] = {0};

        for (int j = 0; j < gradient_count; j++) {
            for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
                average[i] += gradient_buffer[j][i];
            }
        }

        for (int j = 0; j < DEFAULT_MODEL_DIM; j++) {
            average[j] = average[j] / gradient_count;
        }

        printf("Aggregated gradients: [");
        for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
            printf("%.4f", average[i]);
            if (i < DEFAULT_MODEL_DIM - 1) printf(", ");
        }
        printf("]\n");

        printf("Applying weight updates...\n");
        for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
            weights[i] -= LEARNING_RATE * average[i];
        }

        iterations++;
        printf("Weights updated (iteration=%d): [", iterations);
        for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
            printf("%.4f", weights[i]);
            if (i < DEFAULT_MODEL_DIM - 1) printf(", ");
        }
        printf("]\n");

        if (iterations >= MAX_ITERATIONS) {
            printf("Reached MAX_ITERATIONS (%d), broadcasting TERMINATE\n", iterations);
            /*
             * skip sending fresh weights on the final round
             * this avoids an extra client gradient right before shutdown
             */
            broadcast_terminate();
            should_terminate = 1;
        } else {
            broadcast_weights();
        }

        gradient_count = 0;
    }
}


void handle_message(const Message *msg) {
    if (msg == NULL) {
        return;
    }

    switch (msg->header.type) {
        case MSG_JOIN:
            printf("Client joined training\n");
            if (server_get_num_clients(&server_state) >= THRESHOLD) {
                printf("Enough clients connected (%d >= %d), broadcasting initial weights\n", 
                       server_get_num_clients(&server_state), THRESHOLD);
                printf("Initial weights: [");
                for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
                    printf("%.4f", weights[i]);
                    if (i < DEFAULT_MODEL_DIM - 1) printf(", ");
                }
                printf("]\n");
                broadcast_weights();
            }
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


int main(int argc, char **argv) {
    int port = DEFAULT_SERVER_PORT;
    int listening_socket;
    int auto_client_count = 12;

    /* keep the server alive if a client disconnects during broadcast */
    signal(SIGPIPE, SIG_IGN);

    if (argc > 1) {
        if (parse_int_arg(argv[1], 1, 65535, &port) != 0) {
            fprintf(stderr, "Invalid port: %s (expected 1-65535)\n", argv[1]);
            return 1;
        }
    }
    if (argc > 2) {
        if (parse_int_arg(argv[2], 0, INT_MAX, &auto_client_count) != 0) {
            fprintf(stderr, "Invalid client count: %s (expected >= 0)\n", argv[2]);
            return 1;
        }
    }

    printf("Server starting on port %d\n", port);

    listening_socket = server_initialize_listening_socket(port);
    if (listening_socket < 0) {
        fprintf(stderr, "Failed to initialize listening socket\n");
        return 1;
    }

    memset(&server_state, 0, sizeof(server_state));
    server_state.listening_socket = listening_socket;
    server_state.max_fd = listening_socket;
    server_state.running = 1;

    FD_ZERO(&server_state.read_set);
    FD_SET(listening_socket, &server_state.read_set);

    printf("Listening socket initialized and ready for connections\n");
    if (auto_client_count > 0) {
        printf("Auto-spawning %d local clients\n", auto_client_count);
        spawn_local_clients("127.0.0.1", port, auto_client_count);
    }

    server_select_loop(&server_state);
    server_cleanup(&server_state);
    wait_for_spawned_clients();

    printf("Server shutting down\n");
    return 0;
}