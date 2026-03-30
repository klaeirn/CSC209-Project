#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

/*
 * shared constants for the whole project. these stay pretty generic so the
 * protocol and networking code do not care about the exact ml payload yet.
 */

#define DEFAULT_SERVER_PORT 5000
#define MAX_CLIENTS 12 // this is the maximum number of clients that can be connected to the server.
#define MAX_MESSAGE_SIZE 300

/*
 * linear regression will probably send float vectors. keep the model
 * dimension configurable until we settle on an exact model.
 * Threshold is the amount of clients buffer takes in for partial aggregation.
 */
#define DEFAULT_MODEL_DIM 4
#define THRESHOLD 3
#define LEARNING_RATE (0.01f)
#define MAX_ITERATIONS 100

#endif /* common_h */
