/*
 * pipe.h – UNIX pipes header
 * Author: R Andrews – 26 Nov 2025
 */

#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

/* Create a pipe */
int pipe(int pipefd[2]);

#endif /* PIPE_H */
