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

/*
 * Main server program: initializes listening socket and runs select() event loop.
 */

float weights[DEFAULT_MODEL_DIM];
float gradient_buffer[THRESHOLD][DEFAULT_MODEL_DIM];
int gradient_count = 0;
int iterations = 0;
int should_terminate = 0;

ServerState server_state;
static pid_t spawned_client_pids[64];
static int spawned_client_count = 0;

void handle_gradient(const Message *msg);

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
}

void broadcast_terminate() {
    for (int i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
        if (server_state.clients[i].connected) {
            if (protocol_send_terminate(server_state.clients[i].socket_fd) != 0) {
                fprintf(stderr, "Failed to send terminate to client_id=%d (fd=%d)\n",
                        server_state.clients[i].client_id,
                        server_state.clients[i].socket_fd);
            }
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
            /*
             * Kick off training by sending current weights whenever a client joins.
             * Without this, clients wait forever for the first WEIGHTS message.
             */
            broadcast_weights();
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

        if (iterations >= MAX_ITERATIONS) {
            printf("Reached MAX_ITERATIONS (%d), broadcasting TERMINATE\n", iterations);
            /*
             * Do not send a fresh WEIGHTS packet on the terminal round; it can
             * trigger an unnecessary client gradient right before shutdown.
             */
            broadcast_terminate();
            should_terminate = 1;
        } else {
            broadcast_weights();
        }

        gradient_count = 0;
    }
}

int main(int argc, char **argv) {
    int port = DEFAULT_SERVER_PORT;
    int listening_socket;
    int auto_client_count = 12;

    /* Keep server alive if a client disconnects during broadcast. */
    signal(SIGPIPE, SIG_IGN);

    if (argc > 1) {
        port = atoi(argv[1]);
    }
    if (argc > 2) {
        auto_client_count = atoi(argv[2]);
        if (auto_client_count < 0) {
            fprintf(stderr, "Invalid client count: %d\n", auto_client_count);
            return 1;
        }
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
    if (auto_client_count > 0) {
        printf("Auto-spawning %d local clients\n", auto_client_count);
        spawn_local_clients("127.0.0.1", port, auto_client_count);
    }

    /* Run the main select() event loop */
    server_select_loop(&server_state);

    /* Clean up resources */
    server_cleanup(&server_state);
    wait_for_spawned_clients();

    printf("Server shutting down\n");
    return 0;
}