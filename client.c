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
#include <limits.h>

/*
 * client flow
 * - connect to the server
 * - send join
 * - wait for weights
 * - compute a local gradient
 * - send the gradient back
 * - keep going until terminate shows up
 */

// argument format: ./client.c [server host ip][server port][dataset CSV path][delay legnth][delay prob]

// for below, fcntl is a system call to modify fd flags ! it makes it non-blocking
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// connect client socket!
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

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) < 0) return -1;

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

// parse data into array of DataSample
static int parse_csv_row(const char *line, DataSample *sample) {
    float x1, x2, x3, x4, y;
    if (line == NULL || sample == NULL) return -1;
    if (sscanf(line, " %f , %f , %f , %f , %f", &x1, &x2, &x3, &x4, &y) != 5)return -1;

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

    // open the dataset shard
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

    // read every line and keep valid samples
    while (fgets(line, sizeof(line), fp) != NULL) {
        DataSample sample;
        // parse the csv row
        if (parse_csv_row(line, &sample) != 0) continue;

        // grow storage when we run out of room
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

// gradient: first calculate prediction, then error degree
void compute_grad(float *weights, float *grad, float *X, float y, int dim) {
    float prediction = 0.0f;
    for (int i = 0; i < dim; i++) {
        prediction += weights[i] * X[i];
    }

    float error = prediction - y;

    for (int i = 0; i < dim; i++) {
        grad[i] = 2.0f * error * X[i];
    }
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = DEFAULT_SERVER_PORT;
    const char *dataset_path = "data/client_1.csv";
    int socket_fd;
    struct sockaddr_in server_addr;
    float weights[DEFAULT_MODEL_DIM];
    float grad[DEFAULT_MODEL_DIM];
    Message msg;
    ConnState recv_state;
    int running = 1;
    int delay_ms = 0;
    float delay_probability = 0.0f;

    int next_arg = 3;
    DataSample *samples = NULL;
    size_t sample_count = 0;
    size_t sample_index = 0;

    // keep the process alive if server closes while we write
    signal(SIGPIPE, SIG_IGN);

    // optional host + port args
    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);

    // optional dataset path
    if (argc > next_arg) {
        const char *candidate = argv[next_arg];
        int looks_like_path = (strchr(candidate, '/') != NULL) ||
                              (strstr(candidate, ".csv") != NULL);
        if (looks_like_path) {
            // check if the dataset file is readable
            if (access(candidate, R_OK) != 0) {
                fprintf(stderr, "dataset file is not readable: %s\n", candidate);
                return 1;
            }
            dataset_path = candidate;
            next_arg++;
        }
    }

    // optional delay length in milliseconds
    if (argc > next_arg) {
        char *endptr = NULL;
        long parsed_delay = strtol(argv[next_arg], &endptr, 10);
        if (endptr == argv[next_arg] || *endptr != '\0' || parsed_delay < 0 || parsed_delay > INT_MAX) {
            fprintf(stderr, "Invalid delay length (ms): %s\n", argv[next_arg]);
            return 1;
        }
        delay_ms = (int)parsed_delay;
        next_arg++;
    }

    // optional chance of applying delay (0.0 to 1.0) for testing
    if (argc > next_arg) {
        char *endptr = NULL;
        float parsed_probability = strtof(argv[next_arg], &endptr);
        if (endptr == argv[next_arg] || *endptr != '\0' ||
            parsed_probability < 0.0f || parsed_probability > 1.0f) {
            fprintf(stderr, "Invalid delay probability: %s (expected 0.0-1.0)\n", argv[next_arg]);
            return 1;
        }
        delay_probability = parsed_probability;
    }

    // load local samples
    if (load_dataset(dataset_path, &samples, &sample_count) != 0) return 1;

    printf("Client connecting to %s:%d\n", host, port);

    // make socket
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        free(samples);
        return 1;
    }

    // client expects non-blocking behavior
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

    // EINPROGRESS is normal here because connect is non-blocking
    if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0 && errno != EINPROGRESS) {
        perror("connect");
        close(socket_fd);
        free(samples);
        return 1;
    }

    // block briefly until connect actually completes
    if (wait_for_connect(socket_fd) < 0) {
        perror("connect completion");
        close(socket_fd);
        free(samples);
        return 1;
    }

    printf("Connected to server\n");
    if (delay_ms > 0 && delay_probability > 0.0f) {
        printf("Client delay enabled: %dms at probability %.2f\n", delay_ms, delay_probability);
    }

    // recv_state keeps track of partial protocol reads
    protocol_init_conn_state(&recv_state);
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid() ^ (unsigned int)socket_fd);

    memset(weights, 0, sizeof(weights));

    // tell the server this client is ready
    printf("Sending JOIN message\n");
    if (protocol_send_join(socket_fd) != 0) {
        perror("send join");
        close(socket_fd);
        free(samples);
        return 1;
    }

    while (running) {
        // try to read exactly one full protocol message
        int read_result = protocol_read_message(socket_fd, &recv_state, &msg);

        if (read_result == 1) {
            printf("Received message type=%u\n", msg.header.type);

            if (msg.header.type == MSG_WEIGHTS) {
                int dim = 0;
                if (protocol_decode_weights(&msg, weights, &dim) == 0) {
                    DataSample *sample = &samples[sample_index % sample_count];
                    sample_index++;

                    printf("Received weights (dim=%d)\n", dim);

                    // compute and send the grad
                    compute_grad(weights, grad, sample->x, sample->y, DEFAULT_MODEL_DIM);

                    printf("Computed grad: [");
                    for (int i = 0; i < DEFAULT_MODEL_DIM; i++) {
                        printf("%.4f", grad[i]);
                        if (i < DEFAULT_MODEL_DIM - 1) printf(", ");
                    }
                    printf("]\n");

                    // optional "slow client" simulation
                    if (delay_ms > 0 && ((float)rand() / (float)RAND_MAX) < delay_probability) {
                        printf("Simulating client delay: %dms\n", delay_ms);
                        usleep((useconds_t)delay_ms * 1000u);
                    }

                    // send gradient back for aggregation
                    if (protocol_send_grad(socket_fd, grad, DEFAULT_MODEL_DIM) != 0) {
                        if (errno != EPIPE && errno != ECONNRESET) {
                            perror("send grad");
                        }
                        running = 0;
                    } else {
                        printf("Sent grad\n");
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
