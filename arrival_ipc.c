#include "scheduler_api.h"

#include <errno.h>
#include <sys/msg.h>

typedef struct {
    long type;
    ArrivalMessage message;
} QueueMessage;

int arrival_queue_create(void)
{
    return msgget(IPC_PRIVATE, 0600);
}

int arrival_queue_remove(int queue)
{
    return msgctl(queue, IPC_RMID, NULL);
}

static int send_message(int queue, const ArrivalMessage *message, int flags)
{
    QueueMessage packet = { .type = 1, .message = *message };
    return msgsnd(queue, &packet, sizeof packet.message, flags);
}

int arrival_send(int queue, const ArrivalMessage *message)
{
    return send_message(queue, message, 0);
}

int arrival_try_send(int queue, const ArrivalMessage *message)
{
    return send_message(queue, message, IPC_NOWAIT);
}

int arrival_receive(int queue, ArrivalMessage *message)
{
    QueueMessage packet;
    ssize_t size = msgrcv(queue, &packet, sizeof packet.message, 0, 0);
    if (size == -1)
        return -1;
    if ((size_t)size != sizeof packet.message || packet.message.kind < ARRIVAL ||
        packet.message.kind > INPUT_DONE)
    {
        errno = EPROTO;
        return -1;
    }
    *message = packet.message;
    return 0;
}
