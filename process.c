#include "scheduler_api.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_nonnegative(const char *text)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end != '\0' || value < 0 || value > INT_MAX)
        return -1;
    return (int)value;
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s command_fd reply_fd runtime\n", argv[0]);
        return EXIT_FAILURE;
    }

    int command_fd = parse_nonnegative(argv[1]);
    int reply_fd = parse_nonnegative(argv[2]);
    int runtime = parse_nonnegative(argv[3]);
    if (command_fd < 0 || reply_fd < 0 || runtime <= 0)
    {
        fprintf(stderr, "Invalid worker descriptors or runtime\n");
        return EXIT_FAILURE;
    }
    if (worker_serve(command_fd, reply_fd, runtime) == -1)
    {
        perror("Worker tick protocol");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
