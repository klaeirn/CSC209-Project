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
#include <time.h>

float weights[DEFAULT_MODEL_DIM];
float grad_buffer[THRESHOLD][DEFAULT_MODEL_DIM];
int grad_count = 0;
int iterations = 0;
int should_terminate = 0;
static time_t grad_window_start = 0;
static int grad_window_active = 0;
static const int GRADIENT_TIMEOUT_SEC = 3;
static int saw_any_client_join = 0;

ServerState server_state;
static pid_t spawned_client_pids[64];
static int spawned_client_count = 0;

void handle_grad(const Message *msg);
void broadcast_weights(void);
void broadcast_terminate(void);

static void print_curr_weights(const char *prefix) {
    if (prefix != NULL) {
        printf("%s", prefix);
    }
    printf("Current weights at iteration %d: [", iterations);
    for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
        printf("%.4f", weights[i]);
        if (i < DEFAULT_MODEL_DIM - 1) {
            printf(", ");
        }
    }
    printf("]\n");
}

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

static void spawn_local_clients(const char *host, int port, int count, int stress_mode) {
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
            if (stress_mode && i == 1) {
                execl("./client",
                      "./client",
                      host,
                      port_str,
                      shard_path,
                      "2000",
                      "1.0",
                      (char *)NULL);
            } else {
                execl("./client",
                      "./client",
                      host,
                      port_str,
                      shard_path,
                      (char *)NULL);
            }
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

static int queue_client_weights(ClientConnection *client) {
    OutgoingMessage outgoing;
    memset(&outgoing, 0, sizeof(outgoing));

    if (protocol_prepare_weights_outgoing(&outgoing, weights, DEFAULT_MODEL_DIM) != 0) {
        return -1;
    }

    if (client->has_outgoing) {
        protocol_free_outgoing(&client->outgoing_msg);
    }
    client->outgoing_msg = outgoing;
    client->has_outgoing = 1;
    return 0;
}

static int queue_client_terminate(ClientConnection *client) {
    OutgoingMessage outgoing;
    memset(&outgoing, 0, sizeof(outgoing));

    if (protocol_prepare_terminate_outgoing(&outgoing) != 0) {
        return -1;
    }

    if (client->has_outgoing) {
        protocol_free_outgoing(&client->outgoing_msg);
    }
    client->outgoing_msg = outgoing;
    client->has_outgoing = 1;
    return 0;
}

static void aggregate_and_update_from_buffer(void) {
    float average[DEFAULT_MODEL_DIM] = {0};

    printf("Aggregating gradients from %d client(s)...\n", grad_count);

    for (int j = 0; j < grad_count; j++) {
        for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
            average[i] += grad_buffer[j][i];
        }
    }

    for (int j = 0; j < DEFAULT_MODEL_DIM; j++) {
        average[j] = average[j] / grad_count;
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

    grad_count = 0;
    grad_window_active = 0;
    grad_window_start = 0;
}

void broadcast_weights() {
    printf("Broadcasting updated weights to all clients...\n");
    for (int i = 0; i < MAX_CLIENTS_SUPPORTED; i++) {
        if (server_state.clients[i].connected) {
            if (queue_client_weights(&server_state.clients[i]) != 0) {
                fprintf(stderr, "Failed to queue weights for client_id=%d (fd=%d)\n",
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
            if (queue_client_terminate(&server_state.clients[i]) != 0) {
                fprintf(stderr, "Failed to queue terminate for client_id=%d (fd=%d)\n",
                        server_state.clients[i].client_id,
                        server_state.clients[i].socket_fd);
            }
        }
    }
    printf("Terminate broadcast complete. Training finished.\n");
}

void handle_grad(const Message *msg) {
    float grad[DEFAULT_MODEL_DIM] = {0};
    int dim = 0;
    int curr_clients;

    if (protocol_decode_grad(msg, grad, &dim) != 0) {
        fprintf(stderr, "Failed to decode grad\n");
        return;
    }
    if (dim != DEFAULT_MODEL_DIM) {
        fprintf(stderr, "Grad dimension mismatch: expected=%d got=%d\n",
                DEFAULT_MODEL_DIM,
                dim);
        return;
    }

    if (grad_count >= THRESHOLD) {
        fprintf(stderr, "Grad buffer full, dropping grad\n");
        return;
    }

    for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
        grad_buffer[grad_count][i] = grad[i];
    }

    grad_count++;
    printf("Grad %d/%d received\n", grad_count, THRESHOLD);

    if (!grad_window_active) {
        grad_window_start = time(NULL);
        grad_window_active = 1;
    }

    curr_clients = server_get_num_clients(&server_state);
    if (curr_clients > 0 &&
        curr_clients < THRESHOLD &&
        grad_count >= curr_clients) {
        printf("Received gradients from all active clients (%d/%d); aggregating without waiting for timeout\n",
               grad_count,
               curr_clients);
        aggregate_and_update_from_buffer();
        return;
    }

    if (grad_count >= THRESHOLD) {
        aggregate_and_update_from_buffer();
    }
}

void handle_server_tick(void) {
    time_t now;
    double elapsed_sec;
    int curr_clients;

    if (should_terminate) {
        return;
    }

    curr_clients = server_get_num_clients(&server_state);
    if (saw_any_client_join && curr_clients == 0 && iterations < MAX_ITERATIONS) {
        if (grad_count > 0) {
            printf("All clients disconnected with %d pending gradient(s). Discarding partial round.\n",
                   grad_count);
        }
        print_curr_weights("All clients disconnected before training completed. ");
        should_terminate = 1;
        return;
    }

    if (!grad_window_active || grad_count <= 0) {
        return;
    }

    now = time(NULL);
    if (now == (time_t)-1) {
        return;
    }

    elapsed_sec = difftime(now, grad_window_start);
    if (elapsed_sec < GRADIENT_TIMEOUT_SEC) {
        return;
    }

    if (grad_count < THRESHOLD) {
        printf("Gradient timeout reached (%ds): received %d/%d gradients, aggregating partial round\n",
               GRADIENT_TIMEOUT_SEC,
               grad_count,
               THRESHOLD);
    }

    aggregate_and_update_from_buffer();
}

void handle_no_clients_remaining(void) {
    if (should_terminate || !saw_any_client_join || iterations >= MAX_ITERATIONS) {
        return;
    }

    if (grad_count > 0) {
        printf("All clients disconnected with %d pending gradient(s); discarding partial round.\n",
               grad_count);
    }
    print_curr_weights("All clients disconnected before training completed. ");
    should_terminate = 1;
}


void handle_message(const Message *msg) {
    if (msg == NULL) {
        return;
    }

    switch (msg->header.type) {
        case MSG_JOIN:
            printf("Client joined training\n");
            saw_any_client_join = 1;
            if (server_get_num_clients(&server_state) >= THRESHOLD) {
                printf("Enough clients connected (%d >= %d), broadcasting initial weights\n", 
                       server_get_num_clients(&server_state), THRESHOLD);
                print_curr_weights("Initial ");
                broadcast_weights();
            }
            break;

        case MSG_GRADIENT:
            printf("Received gradient\n");
            handle_grad(msg);
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
    int stress_mode = 0;

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
    if (argc > 3) {
        if (parse_int_arg(argv[3], 0, 1, &stress_mode) != 0) {
            fprintf(stderr, "Invalid stress mode: %s (expected 0 or 1)\n", argv[3]);
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
        if (stress_mode) {
            printf("Stress mode enabled: first auto client uses delay=2000ms and probability=1.0\n");
        }
        spawn_local_clients("127.0.0.1", port, auto_client_count, stress_mode);
    }

    server_select_loop(&server_state);
    server_cleanup(&server_state);
    wait_for_spawned_clients();

    printf("Server shutting down\n");
    return 0;
}