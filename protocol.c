#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/*
 * my part of the project: currently includes
 * - packing and unpacking 16 and 32 bit integers (used for the header and so that receiver 
 *   always knows how to start reading a message)
 * - initializing and resetting the connection state
 * - sending and receiving messages (used for the payload)
 * - sending and receiving weights and gradients (used for the payload)
 * - sending and receiving terminate messages (used for the payload)
 * still needs to be fleshed out with the actual payload format. thus the following helpers are not implemented yet:
 * - sending and receiving weights and gradients (used for the payload)
 * - sending and receiving terminate messages (used for the payload)
 * - also decoding helpers for those payloads, not just sending (used for the payload)
 */

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

    memset(&msg, 0, sizeof(msg));
    if (protocol_prepare_outgoing(&msg, type, payload, length) < 0) {
        return -1;
    }

    do {
        flush_status = protocol_flush_outgoing(fd, &msg);
    } while (flush_status == 0);

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

    state->payload_buf = NULL;
    protocol_reset_conn_state(state);
    return 1;
}

int protocol_send_join(int fd) {
    return protocol_send_message(fd, MSG_JOIN, NULL, 0);
}

int protocol_send_weights(int fd, const float *weights, uint32_t dim) {
    (void)fd;
    (void)weights;
    (void)dim;
    errno = ENOSYS;
    return -1;
}

int protocol_send_gradient(int fd, const float *gradient, uint32_t dim) {
    (void)fd;
    (void)gradient;
    (void)dim;
    errno = ENOSYS;
    return -1;
}

int protocol_send_terminate(int fd) {
    return protocol_send_message(fd, MSG_TERMINATE, NULL, 0);
}
