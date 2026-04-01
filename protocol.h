#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

#define PROTOCOL_HEADER_SIZE 6
#define PROTOCOL_VECTOR_DIM DEFAULT_MODEL_DIM
#define PROTOCOL_VECTOR_PAYLOAD_SIZE ((uint32_t)(PROTOCOL_VECTOR_DIM * sizeof(float)))

typedef enum {
    MSG_INVALID = 0,
    MSG_JOIN = 1,
    MSG_WEIGHTS = 2,
    MSG_GRADIENT = 3,
    MSG_TERMINATE = 4
} MessageType;

typedef struct {
    uint16_t type;
    uint32_t length;
} MessageHeader;

typedef struct {
    MessageHeader header;
    uint8_t *payload;
} Message;

typedef enum {
    RECV_READING_HEADER = 0,
    RECV_READING_PAYLOAD = 1
} RecvPhase;

typedef struct {
    RecvPhase phase;
    uint8_t header_buf[PROTOCOL_HEADER_SIZE];
    size_t header_bytes_read;
    uint8_t *payload_buf;
    size_t payload_bytes_read;
    MessageHeader current_header;
} ConnState;

typedef struct {
    uint8_t *buffer;
    size_t total_len;
    size_t bytes_sent;
} OutgoingMessage;

void protocol_init_conn_state(ConnState *state);
void protocol_reset_conn_state(ConnState *state);
void protocol_free_message(Message *msg);

int protocol_pack_header(uint8_t out[PROTOCOL_HEADER_SIZE],
                         uint16_t type,
                         uint32_t length);
int protocol_unpack_header(const uint8_t in[PROTOCOL_HEADER_SIZE],
                           MessageHeader *header);

int protocol_send_message(int fd,
                          uint16_t type,
                          const void *payload,
                          uint32_t length);

int protocol_prepare_outgoing(OutgoingMessage *msg,
                              uint16_t type,
                              const void *payload,
                              uint32_t length);
void protocol_free_outgoing(OutgoingMessage *msg);
int protocol_flush_outgoing(int fd, OutgoingMessage *msg);

int protocol_read_message(int fd, ConnState *state, Message *out_msg);

int protocol_validate_message(const Message *msg);
int protocol_send_join(int fd);
int protocol_send_weights(int fd, const float *weights, uint32_t dim);
int protocol_send_gradient(int fd, const float *gradient, uint32_t dim);
int protocol_send_terminate(int fd);
int protocol_decode_join(const Message *msg);
int protocol_decode_terminate(const Message *msg);
int protocol_decode_weights(const Message *msg, float *weights_out, uint32_t *dim_out);
int protocol_decode_gradient(const Message *msg, float *gradient_out, uint32_t *dim_out);

#endif
