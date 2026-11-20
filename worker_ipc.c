#define _GNU_SOURCE
#include "scheduler_api.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

enum { RUN_TICK = 1 };

/* Return 1 for a full packet, 0 for clean EOF, or -1 on error. */
static int read_packet(int fd, void *data, size_t length)
{
    unsigned char *bytes = data;
    size_t received = 0;
    while (received < length)
    {
        ssize_t count = read(fd, bytes + received, length - received);
        if (count == 0)
        {
            if (received == 0)
                return 0;
            errno = EPROTO;
            return -1;
        }
        if (count > 0)
            received += (size_t)count;
        else if (errno != EINTR)
            return -1;
    }
    return 1;
}

/* Protocol packets fit in PIPE_BUF, so blocking writes are atomic. */
static int write_packet(int fd, const void *data, size_t length)
{
    ssize_t count;
    do {
        count = write(fd, data, length);
    } while (count == -1 && errno == EINTR);
    if (count == (ssize_t)length)
        return 0;
    if (count >= 0)
        errno = EIO;
    return -1;
}

int worker_start(Worker *worker, const char *executable, int runtime)
{
    if (runtime < 1)
    {
        errno = EINVAL;
        return -1;
    }

    enum { COMMAND, REPLY, EXEC_STATUS, PIPE_COUNT };
    int pipes[PIPE_COUNT][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
    for (int i = 0; i < PIPE_COUNT; ++i)
        if (pipe2(pipes[i], O_CLOEXEC) == -1)
            goto fail;

    char command_arg[32], reply_arg[32], runtime_arg[32];
    snprintf(command_arg, sizeof command_arg, "%d", pipes[COMMAND][0]);
    snprintf(reply_arg, sizeof reply_arg, "%d", pipes[REPLY][1]);
    snprintf(runtime_arg, sizeof runtime_arg, "%d", runtime);

    pid_t pid = fork();
    if (pid == -1)
        goto fail;
    if (pid == 0)
    {
        if (fcntl(pipes[COMMAND][0], F_SETFD, 0) != -1 &&
            fcntl(pipes[REPLY][1], F_SETFD, 0) != -1)
            execl(executable, executable, command_arg, reply_arg, runtime_arg, (char *)NULL);
        int error = errno;
        write_packet(pipes[EXEC_STATUS][1], &error, sizeof error);
        _exit(127);
    }

    close(pipes[COMMAND][0]);
    pipes[COMMAND][0] = -1;
    close(pipes[REPLY][1]);
    pipes[REPLY][1] = -1;
    close(pipes[EXEC_STATUS][1]);
    pipes[EXEC_STATUS][1] = -1;

    /* Successful exec closes the status pipe without sending anything. */
    int child_error;
    int status = read_packet(pipes[EXEC_STATUS][0], &child_error, sizeof child_error);
    if (status != 0)
    {
        int error = status == 1 ? child_error : errno;
        if (status == -1)
            kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
            ;
        errno = error;
        goto fail;
    }

    close(pipes[EXEC_STATUS][0]);
    *worker = (Worker){ .pid = pid, .command_fd = pipes[COMMAND][1], .reply_fd = pipes[REPLY][0] };
    return 0;

fail: {
        int error = errno;
        for (int i = 0; i < PIPE_COUNT; ++i)
            for (int end = 0; end < 2; ++end)
                if (pipes[i][end] >= 0)
                    close(pipes[i][end]);
        errno = error;
        return -1;
    }
}

int worker_tick(const Worker *worker, TickAck *ack)
{
    unsigned char command = RUN_TICK;
    if (write_packet(worker->command_fd, &command, sizeof command) == -1)
        return -1;
    int status = read_packet(worker->reply_fd, ack, sizeof *ack);
    if (status == 0)
        errno = EPIPE;
    return status == 1 ? 0 : -1;
}

void worker_close(Worker *worker)
{
    if (worker->command_fd >= 0)
        close(worker->command_fd);
    if (worker->reply_fd >= 0)
        close(worker->reply_fd);
    worker->command_fd = worker->reply_fd = -1;
}

int worker_serve(int command_fd, int reply_fd, int runtime)
{
    if (runtime < 1)
    {
        errno = EINVAL;
        return -1;
    }
    for (int remaining = runtime; remaining > 0; )
    {
        unsigned char command;
        int status = read_packet(command_fd, &command, sizeof command);
        if (status <= 0)
            return status;
        if (command != RUN_TICK)
        {
            errno = EPROTO;
            return -1;
        }
        --remaining;
        TickAck ack = { .remaining = remaining, .finished = remaining == 0 };
        if (write_packet(reply_fd, &ack, sizeof ack) == -1)
            return -1;
    }
    return 0;
}
