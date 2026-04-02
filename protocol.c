#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
/*
 * protocol payload format
 * - join and terminate have no payload
 * - weights and grad always carry exactly protocol_vector_dim floats
 * - each float is sent as a 32-bit ieee-754 bit pattern in network byte order
 *   so payloads stay portable across endianness like big endian and little endian
 */

// binary serialization helpers
// these functions are used to pack and unpack integers and floats into the binary payload
// use htons and htonl to convert the integers so they're not machine dependent
static void pack_u16(uint8_t *dst, uint16_t value) {
    uint16_t net = htons(value);  
    memcpy(dst, &net, sizeof(net)); 
}

static void pack_u32(uint8_t *dst, uint32_t value) {
    uint32_t net = htonl(value); 
    memcpy(dst, &net, sizeof(net));
}

static uint16_t unpack_u16(const uint8_t *src) {
    uint16_t net;
    memcpy(&net, src, sizeof(net));
    return ntohs(net);
}

static uint32_t unpack_u32(const uint8_t *src) {
    uint32_t net;
    memcpy(&net, src, sizeof(net));
    return ntohl(net);
}

static void pack_float32(uint8_t *dst, float value) {
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits)); // no standard hton for floats
    pack_u32(dst, bits);
}

static float unpack_float32(const uint8_t *src) {
    uint32_t bits = unpack_u32(src);
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

// checks the dimension of the vector
static int protocol_validate_vector_dim(int dim) {
    if (dim != PROTOCOL_VECTOR_DIM) {
        fprintf(stderr, "protocol_validate_vector_dim: invalid dimension\n");
        return -1;
    }

    return 0;
}

// turns a vector of floats into a binary payload. make it fixed size like we agreed on
static int protocol_encode_vector_payload(const float *vector, int dim, char payload_out[PROTOCOL_VECTOR_PAYLOAD_SIZE]) {
    int i;
    if (vector == NULL || payload_out == NULL) {
        fprintf(stderr, "protocol_encode_vector_payload: invalid arguments\n");
        return -1; // invalid arguments
    }
    if (protocol_validate_vector_dim(dim) < 0) {
        return -1; // invalid dimension
    }

    for (i = 0; i < PROTOCOL_VECTOR_DIM; i++) { // pack each float into the payload
        pack_float32((uint8_t *)payload_out + (i * sizeof(float)), vector[i]);
    }

    return 0;
}

// same idea as the func above but for decoding
static int protocol_decode_vector_payload(const Message *msg, int expected_type, float *vector_out, int *dim_out) {
    int i;
    if (msg == NULL || vector_out == NULL || dim_out == NULL) {
        fprintf(stderr, "protocol_decode_vector_payload: invalid arguments\n");
        return -1;
    }
    if (protocol_validate_message(msg) < 0) {
        return -1;
    }
    if (msg->header.type != expected_type) {
        fprintf(stderr, "protocol_decode_vector_payload: message type mismatch\n");
        return -1;
    }
    for (i = 0; i < PROTOCOL_VECTOR_DIM; i++) {
        vector_out[i] = unpack_float32((const uint8_t *)msg->payload +
                                       (i * sizeof(float)));
    }
    *dim_out = PROTOCOL_VECTOR_DIM;
    return 0;
}

// zeroes and sets up connection state for incremental reads
void protocol_init_conn_state(ConnState *state) {
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state)); // zero out the memory
    state->phase = RECV_READING_HEADER; // start reading the header
}

// clears in progress receive state for a connection without zeroing all of ConnState
void protocol_reset_conn_state(ConnState *state) {
    if (state == NULL) {
        return;
    }
    free(state->payload_buf);
    // reset to default values
    state->payload_buf = NULL;
    state->payload_bytes_read = 0;
    state->header_bytes_read = 0;
    state->curr_header.type = MSG_INVALID;
    state->curr_header.length = 0;
    state->phase = RECV_READING_HEADER;
}

// frees heap memory for a message's payload buffer
void protocol_free_message(Message *msg) {
    if (msg == NULL) {
        return;
    }
    free(msg->payload);
    msg->payload = NULL;
    msg->header.type = MSG_INVALID;
    msg->header.length = 0;
}

// writes a binary message header into the output buffer
// keeps the format fixed so we can always parse it correctly
int protocol_pack_header(char out[PROTOCOL_HEADER_SIZE],
                         uint16_t type,
                         uint32_t length) {
    if (out == NULL) {
        return -1;
    }
    pack_u16((uint8_t *)out, type); // pack the type
    pack_u32((uint8_t *)out + 2, length); // pack the length
    return 0;
}

int protocol_unpack_header(const char in[PROTOCOL_HEADER_SIZE],
                           MessageHeader *header) {
    if (in == NULL || header == NULL) {
        return -1;
    }
    header->type = unpack_u16((const uint8_t *)in);
    header->length = unpack_u32((const uint8_t *)in + 2);
    return 0;
}

// builds a complete outgoing message in memory before sending on the socket
int protocol_prepare_outgoing(OutgoingMessage *msg,
                              uint16_t type,
                              const void *payload,
                              uint32_t length) {
    // allocate memory for the message
    msg->buffer = malloc(PROTOCOL_HEADER_SIZE + length);
    if (msg->buffer == NULL) {
        return -1; 
    }
    // pack the header
    if (protocol_pack_header(msg->buffer, type, length) < 0) {
        free(msg->buffer);
        msg->buffer = NULL;
        return -1;
    }
    // copy the payload
    memcpy(msg->buffer + PROTOCOL_HEADER_SIZE, payload, length);
    msg->total_len = PROTOCOL_HEADER_SIZE + length;
    msg->bytes_sent = 0;
    return 0;
}

// same idea as the func above but for weights
int protocol_prepare_weights_outgoing(OutgoingMessage *msg,
                                      const float *weights,
                                      int dim) {
    char payload[PROTOCOL_VECTOR_PAYLOAD_SIZE];
    // encode the weights
    if (protocol_encode_vector_payload(weights, dim, payload) < 0) {
        return -1;
    }
    // prepare the outgoing message
    return protocol_prepare_outgoing(msg, MSG_WEIGHTS, payload, PROTOCOL_VECTOR_PAYLOAD_SIZE);
}

// same idea as the func above but for terminate
int protocol_prepare_terminate_outgoing(OutgoingMessage *msg) {
    return protocol_prepare_outgoing(msg, MSG_TERMINATE, NULL, 0);
}

// frees the memory for an outgoing message
void protocol_free_outgoing(OutgoingMessage *msg) {
    free(msg->buffer);
    msg->buffer = NULL;
    msg->total_len = 0;
    msg->bytes_sent = 0;
}

// tries to send more of a prepared outgoing message on socket
int protocol_flush_outgoing(int fd, OutgoingMessage *msg) {
    ssize_t n_written;
    size_t remaining;

    remaining = msg->total_len - msg->bytes_sent;
    n_written = write(fd, msg->buffer + msg->bytes_sent, remaining);
    if (n_written < 0) {
        perror("protocol_flush_outgoing");
        return -1;
    }

    msg->bytes_sent += n_written;
    if (msg->bytes_sent == msg->total_len) {
        return 1;
    }
    return 0;
}

// keeps sending the message until it's all sent or there's an error
int protocol_send_message(int fd,
                          uint16_t type,
                          const void *payload,
                          uint32_t length) {
    // initialize the outgoing message
    OutgoingMessage msg;
    int flush_status;
    int select_status;
    fd_set write_fds;
    struct timeval tv;
    int timeout_count = 0;
    const int max_timeouts = 5;

    memset(&msg, 0, sizeof(msg));
    // prepare the outgoing message
    if (protocol_prepare_outgoing(&msg, type, payload, length) < 0) {
        return -1;
    }
    // keep sending the message until itts all sent or there an error
    flush_status = 0;
    while (flush_status == 0) {
        // select the socket for writing
        FD_ZERO(&write_fds);
        // add the socket to the set
        FD_SET(fd, &write_fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        // wait for the socket to be writable
        select_status = select(fd + 1, NULL, &write_fds, NULL, &tv);
        // if select fail do an error and break
        if (select_status < 0) {
            fprintf(stderr, "protocol_send_message: select failed\n");
            flush_status = -1;
            break;
        }
        if (select_status == 0) {
            // if select timed out increment the timeout count
            timeout_count++;
            if (timeout_count >= max_timeouts) {
                fprintf(stderr, "protocol_send_message: select timed out\n");
                flush_status = -1;
                break; // timed out
            }
            continue;
        }

        // reset the timeout count
        timeout_count = 0;
        // flush the outgoing message
        flush_status = protocol_flush_outgoing(fd, &msg);
    }

    protocol_free_outgoing(&msg); // free the outgoing message bc it's completely sent or error
    if (flush_status < 0) {
        return -1;
    } else {
        return 0;
    }
}

// reads a full incoming message from the socket in chunks so callers can finish it across multiple calls
int protocol_read_message(int fd, ConnState *state, Message *out_msg) {
    ssize_t n_read;
    size_t remaining;
    // read the header
    if (state->phase == RECV_READING_HEADER) {
        remaining = PROTOCOL_HEADER_SIZE - state->header_bytes_read; // remaining bytes to read
        n_read = read(fd, state->header_buf + state->header_bytes_read, remaining);
        if (n_read == 0) {
            return -1;
        }
        if (n_read < 0) {
            return 0;
        }
        state->header_bytes_read += n_read;
        if (state->header_bytes_read < PROTOCOL_HEADER_SIZE) {
            return 0;
        }
        if (protocol_unpack_header(state->header_buf, &state->curr_header) < 0) {
            return -1;
        }
        // check the message size
        if (state->curr_header.length > MAX_MESSAGE_SIZE) {
            fprintf(stderr, "protocol_read_message: message size too large\n");
            return -1;
        }
        if (state->curr_header.length == 0) {
            out_msg->header = state->curr_header;
            out_msg->payload = NULL;
            if (protocol_validate_message(out_msg) < 0) {
                protocol_reset_conn_state(state);
                return -1;
            }
            protocol_reset_conn_state(state);
            return 1;
        }
        // allocate memory for the payload
        state->payload_buf = malloc(state->curr_header.length);
        if (state->payload_buf == NULL) {
            return -1;
        }
        state->phase = RECV_READING_PAYLOAD;
    }
    // read the remaining bytes
    remaining = state->curr_header.length - state->payload_bytes_read; // remaining bytes to read
    n_read = read(fd, state->payload_buf + state->payload_bytes_read, remaining);
    if (n_read == 0) {
        return -1;
    }
    if (n_read < 0) {
        return 0;
    }
    state->payload_bytes_read += n_read;
    if (state->payload_bytes_read < state->curr_header.length) {
        return 0;
    }
    // set the message header and payload
    out_msg->header = state->curr_header;
    out_msg->payload = state->payload_buf;
    // validate the message
    if (protocol_validate_message(out_msg) < 0) {
        // free the payload buffer
        free(state->payload_buf);
        state->payload_buf = NULL;
        protocol_reset_conn_state(state);
        return -1;
    }
    state->payload_buf = NULL;
    protocol_reset_conn_state(state);
    return 1;
}

// checks for a consistent message format (type, length, payload)
int protocol_validate_message(const Message *msg) {
    if (msg == NULL) {
        fprintf(stderr, "protocol_validate_message: invalid message\n");
        return -1;
    }

    if (msg->header.type == MSG_JOIN || msg->header.type == MSG_TERMINATE) {
        if (msg->header.length != 0 || msg->payload != NULL) {
            fprintf(stderr, "protocol_validate_message: message type mismatch\n");
            return -1;
        }
        return 0;
    }

    if (msg->header.type == MSG_WEIGHTS || msg->header.type == MSG_GRADIENT) {
        if (msg->header.length != PROTOCOL_VECTOR_PAYLOAD_SIZE || msg->payload == NULL) {
            fprintf(stderr, "protocol_validate_message: message type mismatch\n");
            return -1;
        }
        return 0;
    }

    fprintf(stderr, "protocol_validate_message: unsupported message type\n");
    return -1;
}

// helpers to send MSG_JOIN, MSG_WEIGHTS, MSG_GRADIENT, and MSG_TERMINATE on fd
int protocol_send_join(int fd) {
    // send the join message
    return protocol_send_message(fd, MSG_JOIN, NULL, 0);
}

int protocol_send_weights(int fd, const float *weights, int dim) {
    char payload[PROTOCOL_VECTOR_PAYLOAD_SIZE];
    // encode the weights
    if (protocol_encode_vector_payload(weights, dim, payload) < 0) {
        return -1;
    }
    // send the weights message
    return protocol_send_message(fd, MSG_WEIGHTS, payload, PROTOCOL_VECTOR_PAYLOAD_SIZE);
}

int protocol_send_grad(int fd, const float *grad, int dim) {
    char payload[PROTOCOL_VECTOR_PAYLOAD_SIZE];
    // encode the gradient
    if (protocol_encode_vector_payload(grad, dim, payload) < 0) {
        return -1;
    }
    // send the gradient message
    return protocol_send_message(fd, MSG_GRADIENT, payload, PROTOCOL_VECTOR_PAYLOAD_SIZE);
}

int protocol_send_terminate(int fd) {
    // send the terminate message
    return protocol_send_message(fd, MSG_TERMINATE, NULL, 0);
}

// similarly, functions below check that the received message is valid and of the correct type
int protocol_decode_join(const Message *msg) {
    if (protocol_validate_message(msg) < 0) {
        return -1;
    }
    if (msg->header.type != MSG_JOIN) {
        fprintf(stderr, "protocol_decode_join: message type mismatch\n");
        return -1;
    }
    return 0;
}

int protocol_decode_terminate(const Message *msg) {
    if (protocol_validate_message(msg) < 0) {
        return -1;
    }
    if (msg->header.type != MSG_TERMINATE) {
        fprintf(stderr, "protocol_decode_terminate: message type mismatch\n");
        return -1;
    }
    return 0;
}

int protocol_decode_weights(const Message *msg, float *weights_out, int *dim_out) {
    return protocol_decode_vector_payload(msg, MSG_WEIGHTS, weights_out, dim_out);
}

int protocol_decode_grad(const Message *msg, float *grad_out, int *dim_out) {
    return protocol_decode_vector_payload(msg, MSG_GRADIENT, grad_out, dim_out);
}