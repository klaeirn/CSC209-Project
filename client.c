#include "common.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

/*
 * client:
 * - connect to the server
 * - send join
 * - wait for weights
 * - compute a local gradient
 * - send gradient back
 * - keep going until terminate shows up
 */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

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
    int socket_fd;
    struct sockaddr_in server_addr;
    float weights[DEFAULT_MODEL_DIM];
    float gradient[DEFAULT_MODEL_DIM];
    float X[DEFAULT_MODEL_DIM];
    float y;
    Message msg;
    ConnState recv_state;
    int running = 1;

    int delay_ms = 0;
    float delay_probability = 0.0f;

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = atoi(argv[2]);
    }
    if (argc > 3) {
        delay_ms = atoi(argv[3]); 
    }
    if (argc > 4) {
        delay_probability = atof(argv[4]); 
        if (delay_probability < 0.0f) delay_probability = 0.0f;
        if (delay_probability > 1.0f) delay_probability = 1.0f;
    }

    printf("Client connecting to %s:%d\n", host, port);

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        return 1;
    }

    if (set_nonblocking(socket_fd) < 0) {
        perror("fcntl");
        close(socket_fd);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(socket_fd);
        return 1;
    }

    if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        if (errno != EINPROGRESS) {
            perror("connect");
            close(socket_fd);
            return 1;
        }
    }

    printf("Connected to server\n");
    srand((unsigned)time(NULL) ^ (unsigned)socket_fd);

    protocol_init_conn_state(&recv_state);

    memset(weights, 0, sizeof(weights));

    printf("Sending JOIN message\n");
    protocol_send_join(socket_fd);

    while (running) {
        int read_result = protocol_read_message(socket_fd, &recv_state, &msg);

        if (read_result == 1) {
            printf("Received message type=%u\n", msg.header.type);

            if (msg.header.type == MSG_WEIGHTS) {
                uint32_t dim = 0;
                if (protocol_decode_weights(&msg, weights, &dim) == 0) {
                    printf("Received weights (dim=%u)\n", dim);

                    for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
                        X[i] = (float)rand() / RAND_MAX;
                    }
                    y = (float)rand() / RAND_MAX;

                    compute_gradient(weights, gradient, X, y, DEFAULT_MODEL_DIM);

                    printf("Computed gradient: [");
                    for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
                        printf("%.4f", gradient[i]);
                        if (i < DEFAULT_MODEL_DIM - 1) printf(", ");
                    }
                    printf("]\n");

                    if (delay_ms > 0 && ((float)rand() / RAND_MAX) < delay_probability) {
                        printf("Simulating client delay: %dms\n", delay_ms);
                        usleep(delay_ms * 1000);
                    }

                    protocol_send_gradient(socket_fd, gradient, DEFAULT_MODEL_DIM);
                    printf("Sent gradient\n");
                }

            } else if (msg.header.type == MSG_TERMINATE) {
                printf("Received TERMINATE message\n");
                running = 0;
            }

            protocol_free_message(&msg);
        } else if (read_result == -1) {
            printf("Disconnected from server\n");
            running = 0;
        }

        usleep(100000); 
    }

    close(socket_fd);
    printf("Client shutting down\n");
    return 0;
}
