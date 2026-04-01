#include "common.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

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

static int wait_for_connect(int fd) {
    fd_set write_fds;
    struct timeval tv;
    int ready;
    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);

    FD_ZERO(&write_fds);
    FD_SET(fd, &write_fds);
    tv.tv_sec = 5;
    tv.tv_usec = 0;

    ready = select(fd + 1, NULL, &write_fds, NULL, &tv);
    if (ready <= 0) {
        if (ready == 0) {
            errno = ETIMEDOUT;
        }
        return -1;
    }

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) < 0) {
        return -1;
    }
    if (so_error != 0) {
        errno = so_error;
        return -1;
    }

    return 0;
}

typedef struct {
    float x[DEFAULT_MODEL_DIM];
    float y;
} DataSample;

static int parse_csv_row(const char *line, DataSample *sample) {
    float x1, x2, x3, x4, y;
    if (line == NULL || sample == NULL) {
        return -1;
    }
    if (sscanf(line, " %f , %f , %f , %f , %f", &x1, &x2, &x3, &x4, &y) != 5) {
        return -1;
    }
    sample->x[0] = x1;
    sample->x[1] = x2;
    sample->x[2] = x3;
    sample->x[3] = x4;
    sample->y = y;
    return 0;
}

static int load_dataset(const char *path, DataSample **samples_out, size_t *count_out) {
    FILE *fp;
    DataSample *samples;
    size_t count = 0;
    size_t capacity = 16;
    char line[256];

    if (path == NULL || samples_out == NULL || count_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        perror("fopen dataset");
        return -1;
    }

    samples = malloc(capacity * sizeof(*samples));
    if (samples == NULL) {
        fclose(fp);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        DataSample sample;
        char *trim = line;
        while (*trim == ' ' || *trim == '\t') {
            trim++;
        }
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '#') {
            continue;
        }
        if (strncmp(trim, "x1,", 3) == 0) {
            continue;
        }
        if (parse_csv_row(line, &sample) != 0) {
            fprintf(stderr, "skipping invalid CSV row: %s", line);
            continue;
        }
        if (count == capacity) {
            DataSample *grown = realloc(samples, (capacity * 2) * sizeof(*samples));
            if (grown == NULL) {
                free(samples);
                fclose(fp);
                return -1;
            }
            samples = grown;
            capacity *= 2;
        }
        samples[count++] = sample;
    }

    fclose(fp);

    if (count == 0) {
        free(samples);
        fprintf(stderr, "dataset has no valid rows: %s\n", path);
        errno = EINVAL;
        return -1;
    }

    *samples_out = samples;
    *count_out = count;
    return 0;
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
    const char *dataset_path = "data/client_1.csv";
    int socket_fd;
    struct sockaddr_in server_addr;
    float weights[DEFAULT_MODEL_DIM];
    float gradient[DEFAULT_MODEL_DIM];
    Message msg;
    ConnState recv_state;
    int running = 1;

    int delay_ms = 0;
    float delay_probability = 0.0f;
    int next_arg = 3;
    DataSample *samples = NULL;
    size_t sample_count = 0;
    size_t sample_index = 0;

    /* Prevent process termination on write() to a closed socket. */
    signal(SIGPIPE, SIG_IGN);

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = atoi(argv[2]);
    }
    if (argc > next_arg) {
        const char *candidate = argv[next_arg];
        int looks_like_path = (strchr(candidate, '/') != NULL) ||
                              (strstr(candidate, ".csv") != NULL);
        if (looks_like_path) {
            if (access(candidate, R_OK) != 0) {
                fprintf(stderr, "dataset file is not readable: %s\n", candidate);
                return 1;
            }
            dataset_path = candidate;
            next_arg++;
        }
    }
    if (argc > next_arg) {
        delay_ms = atoi(argv[next_arg]); /* delay in milliseconds for straggler simulation */
        next_arg++;
    }
    if (argc > next_arg) {
        delay_probability = atof(argv[next_arg]); /* 0.0-1.0 chance to delay each gradient send */
        if (delay_probability < 0.0f) delay_probability = 0.0f;
        if (delay_probability > 1.0f) delay_probability = 1.0f;
    }

    if (load_dataset(dataset_path, &samples, &sample_count) != 0) {
        return 1;
    }

    printf("Client connecting to %s:%d using %s (%zu samples)\n",
           host,
           port,
           dataset_path,
           sample_count);

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        free(samples);
        return 1;
    }

    if (set_nonblocking(socket_fd) < 0) {
        perror("fcntl");
        close(socket_fd);
        free(samples);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(socket_fd);
        free(samples);
        return 1;
    }

    if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0 &&
        errno != EINPROGRESS) {
        perror("connect");
        close(socket_fd);
        free(samples);
        return 1;
    }

    if (wait_for_connect(socket_fd) < 0) {
        perror("connect completion");
        close(socket_fd);
        free(samples);
        return 1;
    }

    printf("Connected to server\n");
    srand((unsigned)time(NULL) ^ (unsigned)socket_fd);

    protocol_init_conn_state(&recv_state);

    memset(weights, 0, sizeof(weights));

    printf("Sending JOIN message\n");
    if (protocol_send_join(socket_fd) != 0) {
        perror("send join");
        close(socket_fd);
        free(samples);
        return 1;
    }

    while (running) {
        int read_result = protocol_read_message(socket_fd, &recv_state, &msg);

        if (read_result == 1) {
            printf("Received message type=%u\n", msg.header.type);

            if (msg.header.type == MSG_WEIGHTS) {
                uint32_t dim = 0;
                if (protocol_decode_weights(&msg, weights, &dim) == 0) {
                    DataSample *sample = &samples[sample_index % sample_count];
                    sample_index++;

                    printf("Received weights (dim=%u)\n", dim);

                    /* Compute and send gradient */
                    compute_gradient(weights, gradient, sample->x, sample->y, DEFAULT_MODEL_DIM);

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

                    if (protocol_send_gradient(socket_fd, gradient, DEFAULT_MODEL_DIM) != 0) {
                        if (errno != EPIPE && errno != ECONNRESET) {
                            perror("send gradient");
                        }
                        running = 0;
                    } else {
                        printf("Sent gradient\n");
                    }
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
    free(samples);
    printf("Client shutting down\n");
    return 0;
}
