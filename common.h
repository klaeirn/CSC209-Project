#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

/*
 * shared constants for the whole project
 * keeping these generic so protocol and networking code stay decoupled from the exact ml payload for now
 */

#define DEFAULT_SERVER_PORT 4242
#define MAX_CLIENTS 12
#define MAX_MESSAGE_SIZE 300

#define DEFAULT_MODEL_DIM 4
#define THRESHOLD 3
#define LEARNING_RATE (0.01f)
#define MAX_ITERATIONS 100

#endif
