#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

/*
 * shared constants for the whole project. these stay pretty generic so the
 * protocol and networking code do not care about the exact ml payload yet.
 */

#define DEFAULT_SERVER_PORT 5000
#define MAX_CLIENTS 12
#define MAX_MESSAGE_SIZE 300

#define DEFAULT_MODEL_DIM 4
#define THRESHOLD 3
#define LEARNING_RATE (0.01f)
#define MAX_ITERATIONS 100

#endif
