#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
/*
 * protocol payload format
 * - join and terminate have no payload
 * - weights and gradient always carry exactly protocol_vector_dim floats
 * - each float is sent as a 32-bit ieee-754 bit pattern in network byte order
 *   so payloads stay portable across endianness like big endian and little endian
 */

_Static_assert(sizeof(float) == sizeof(uint32_t),
               "protocol float encoding requires 32-bit float");

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

    memcpy(&bits, &value, sizeof(bits));
    pack_u32(dst, bits);
}

static float unpack_float32(const uint8_t *src) {
    uint32_t bits = unpack_u32(src);
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int protocol_validate_vector_dim(uint32_t dim) {
    if (dim != PROTOCOL_VECTOR_DIM) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static int protocol_encode_vector_payload(const float *vector,
                                          uint32_t dim,
                                          uint8_t payload_out[PROTOCOL_VECTOR_PAYLOAD_SIZE]) {
    uint32_t i;
    if (vector == NULL || payload_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (protocol_validate_vector_dim(dim) < 0) {
        return -1;
    }

    for (i = 0; i < PROTOCOL_VECTOR_DIM; i++) {
        pack_float32(payload_out + (i * sizeof(float)), vector[i]);
    }

    return 0;
}

static int protocol_decode_vector_payload(const Message *msg,
                                          uint16_t expected_type,
                                          float *vector_out,
                                          uint32_t *dim_out) {
    uint32_t i;
    if (msg == NULL || vector_out == NULL || dim_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (protocol_validate_message(msg) < 0) {
        return -1;
    }
    if (msg->header.type != expected_type) {
        errno = EPROTO;
        return -1;
    }
    for (i = 0; i < PROTOCOL_VECTOR_DIM; i++) {
        vector_out[i] = unpack_float32(msg->payload + (i * sizeof(float)));
    }
    *dim_out = PROTOCOL_VECTOR_DIM;
    return 0;
}

void protocol_init_conn_state(ConnState *state) {
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->phase = RECV_READING_HEADER;
}

void protocol_reset_conn_state(ConnState *state) {
    if (state == NULL) {
        return;
    }
    free(state->payload_buf);
    state->payload_buf = NULL;
    state->payload_bytes_read = 0;
    state->header_bytes_read = 0;
    state->current_header.type = MSG_INVALID;
    state->current_header.length = 0;
    state->phase = RECV_READING_HEADER;
}

void protocol_free_message(Message *msg) {
    if (msg == NULL) {
        return;
    }
    free(msg->payload);
    msg->payload = NULL;
    msg->header.type = MSG_INVALID;
    msg->header.length = 0;
}

int protocol_pack_header(uint8_t out[PROTOCOL_HEADER_SIZE],
                         uint16_t type,
                         uint32_t length) {
    if (out == NULL) {
        return -1;
    }
    pack_u16(out, type);
    pack_u32(out + 2, length);
    return 0;
}

int protocol_unpack_header(const uint8_t in[PROTOCOL_HEADER_SIZE],
                           MessageHeader *header) {
    if (in == NULL || header == NULL) {
        return -1;
    }
    header->type = unpack_u16(in);
    header->length = unpack_u32(in + 2);
    return 0;
}

int protocol_prepare_outgoing(OutgoingMessage *msg,
                              uint16_t type,
                              const void *payload,
                              uint32_t length) {
    if (msg == NULL) {
        return -1;
    }
    if (length > MAX_MESSAGE_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }
    msg->buffer = malloc(PROTOCOL_HEADER_SIZE + length);
    if (msg->buffer == NULL) {
        return -1;
    }
    if (protocol_pack_header(msg->buffer, type, length) < 0) {
        free(msg->buffer);
        msg->buffer = NULL;
        return -1;
    }
    if (length > 0 && payload != NULL) {
        memcpy(msg->buffer + PROTOCOL_HEADER_SIZE, payload, length);
    }
    msg->total_len = PROTOCOL_HEADER_SIZE + length;
    msg->bytes_sent = 0;
    return 0;
}

int protocol_prepare_weights_outgoing(OutgoingMessage *msg,
                                      const float *weights,
                                      uint32_t dim) {
    uint8_t payload[PROTOCOL_VECTOR_PAYLOAD_SIZE];
    if (protocol_encode_vector_payload(weights, dim, payload) < 0) {
        return -1;
    }
    return protocol_prepare_outgoing(msg, MSG_WEIGHTS, payload, PROTOCOL_VECTOR_PAYLOAD_SIZE);
}

int protocol_prepare_terminate_outgoing(OutgoingMessage *msg) {
    return protocol_prepare_outgoing(msg, MSG_TERMINATE, NULL, 0);
}

void protocol_free_outgoing(OutgoingMessage *msg) {
    if (msg == NULL) {
        return;
    }

    free(msg->buffer);
    msg->buffer = NULL;
    msg->total_len = 0;
    msg->bytes_sent = 0;
}

int protocol_flush_outgoing(int fd, OutgoingMessage *msg) {
    ssize_t n_written;
    size_t remaining;

    if (msg == NULL || msg->buffer == NULL) {
        return -1;
    }

    remaining = msg->total_len - msg->bytes_sent;
    n_written = write(fd, msg->buffer + msg->bytes_sent, remaining);
    if (n_written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }

    msg->bytes_sent += (size_t)n_written;
    if (msg->bytes_sent == msg->total_len) {
        return 1;
    }
    return 0;
}

int protocol_send_message(int fd,
                          uint16_t type,
                          const void *payload,
                          uint32_t length) {
    OutgoingMessage msg;
    int flush_status;
    int select_status;
    fd_set write_fds;
    struct timeval tv;
    int timeout_count = 0;
    const int max_timeouts = 5;

    memset(&msg, 0, sizeof(msg));
    if (protocol_prepare_outgoing(&msg, type, payload, length) < 0) {
        return -1;
    }
    flush_status = protocol_flush_outgoing(fd, &msg);
    while (flush_status == 0) {
        FD_ZERO(&write_fds);
        FD_SET(fd, &write_fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        select_status = select(fd + 1, NULL, &write_fds, NULL, &tv);
        if (select_status < 0) {
            if (errno == EINTR) {
                continue;
            }
            flush_status = -1;
            break;
        }
        if (select_status == 0) {
            timeout_count++;
            if (timeout_count >= max_timeouts) {
                errno = ETIMEDOUT;
                flush_status = -1;
                break;
            }
            continue;
        }

        timeout_count = 0;
        flush_status = protocol_flush_outgoing(fd, &msg);
    }

    protocol_free_outgoing(&msg);
    return (flush_status < 0) ? -1 : 0;
}

int protocol_read_message(int fd, ConnState *state, Message *out_msg) {
    ssize_t n_read;
    size_t remaining;

    if (state == NULL || out_msg == NULL) {
        return -1;
    }
    if (state->phase == RECV_READING_HEADER) {
        remaining = PROTOCOL_HEADER_SIZE - state->header_bytes_read;
        n_read = read(fd, state->header_buf + state->header_bytes_read, remaining);
        if (n_read == 0) {
            return -1;
        }
        if (n_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            return -1;
        }
        state->header_bytes_read += (size_t)n_read;
        if (state->header_bytes_read < PROTOCOL_HEADER_SIZE) {
            return 0;
        }
        if (protocol_unpack_header(state->header_buf, &state->current_header) < 0) {
            return -1;
        }
        if (state->current_header.length > MAX_MESSAGE_SIZE) {
            errno = EMSGSIZE;
            return -1;
        }
        if (state->current_header.length == 0) {
            out_msg->header = state->current_header;
            out_msg->payload = NULL;
            if (protocol_validate_message(out_msg) < 0) {
                protocol_reset_conn_state(state);
                return -1;
            }
            protocol_reset_conn_state(state);
            return 1;
        }
        state->payload_buf = malloc(state->current_header.length);
        if (state->payload_buf == NULL) {
            return -1;
        }
        state->phase = RECV_READING_PAYLOAD;
    }

    remaining = state->current_header.length - state->payload_bytes_read;
    n_read = read(fd, state->payload_buf + state->payload_bytes_read, remaining);
    if (n_read == 0) {
        return -1;
    }
    if (n_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
    state->payload_bytes_read += (size_t)n_read;
    if (state->payload_bytes_read < state->current_header.length) {
        return 0;
    }
    out_msg->header = state->current_header;
    out_msg->payload = state->payload_buf;
    if (protocol_validate_message(out_msg) < 0) {
        free(state->payload_buf);
        state->payload_buf = NULL;
        protocol_reset_conn_state(state);
        return -1;
    }
    state->payload_buf = NULL;
    protocol_reset_conn_state(state);
    return 1;
}

int protocol_validate_message(const Message *msg) {
    if (msg == NULL) {
        errno = EINVAL;
        return -1;
    }
    switch (msg->header.type) {
    case MSG_JOIN:
    case MSG_TERMINATE:
        if (msg->header.length != 0 || msg->payload != NULL) {
            errno = EBADMSG;
            return -1;
        }
        return 0;
    case MSG_WEIGHTS:
    case MSG_GRADIENT:
        if (msg->header.length != PROTOCOL_VECTOR_PAYLOAD_SIZE || msg->payload == NULL) {
            errno = EBADMSG;
            return -1;
        }
        return 0;
    default:
        errno = EPROTO;
        return -1;
    }
}

int protocol_send_join(int fd) {
    return protocol_send_message(fd, MSG_JOIN, NULL, 0);
}

int protocol_send_weights(int fd, const float *weights, uint32_t dim) {
    uint8_t payload[PROTOCOL_VECTOR_PAYLOAD_SIZE];
    if (protocol_encode_vector_payload(weights, dim, payload) < 0) {
        return -1;
    }
    return protocol_send_message(fd, MSG_WEIGHTS, payload, PROTOCOL_VECTOR_PAYLOAD_SIZE);
}

int protocol_send_gradient(int fd, const float *gradient, uint32_t dim) {
    uint8_t payload[PROTOCOL_VECTOR_PAYLOAD_SIZE];
    if (protocol_encode_vector_payload(gradient, dim, payload) < 0) {
        return -1;
    }
    return protocol_send_message(fd, MSG_GRADIENT, payload, PROTOCOL_VECTOR_PAYLOAD_SIZE);
}

int protocol_send_terminate(int fd) {
    return protocol_send_message(fd, MSG_TERMINATE, NULL, 0);
}

int protocol_decode_join(const Message *msg) {
    if (protocol_validate_message(msg) < 0) {
        return -1;
    }
    if (msg->header.type != MSG_JOIN) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int protocol_decode_terminate(const Message *msg) {
    if (protocol_validate_message(msg) < 0) {
        return -1;
    }
    if (msg->header.type != MSG_TERMINATE) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int protocol_decode_weights(const Message *msg, float *weights_out, uint32_t *dim_out) {
    return protocol_decode_vector_payload(msg, MSG_WEIGHTS, weights_out, dim_out);
}

int protocol_decode_gradient(const Message *msg, float *gradient_out, uint32_t *dim_out) {
    return protocol_decode_vector_payload(msg, MSG_GRADIENT, gradient_out, dim_out);
}