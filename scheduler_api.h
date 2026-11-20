#ifndef SCHEDULER_API_H
#define SCHEDULER_API_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int id;
    int arrival;
    int runtime;
    int priority;
} JobSpec;

typedef enum { ARRIVAL = 1, TICK_DONE, INPUT_DONE } ArrivalKind;

typedef struct {
    ArrivalKind kind;
    int tick;
    JobSpec job; /* Present only for ARRIVAL. */
} ArrivalMessage;

/* Create in the generator, pass the queue ID to the scheduler, and remove
 * in the generator. Send arrivals by ID, then TICK_DONE, even for empty ticks.
 * Send INPUT_DONE after the final batch. Calls return -1 with errno on failure;
 * an interrupted queue operation returns EINTR to the caller. */
int arrival_queue_create(void);
int arrival_queue_remove(int queue);
int arrival_send(int queue, const ArrivalMessage *message);
int arrival_receive(int queue, ArrivalMessage *message);

typedef struct {
    int remaining;
    int finished;
} TickAck;

typedef struct {
    pid_t pid;
    int command_fd;
    int reply_fd;
} Worker;

/* Launch process.out. The scheduler should ignore SIGPIPE so a broken pipe
 * returns EPIPE. One synchronous worker_tick accounts for one CPU interval.
 * Call again only after a successful non-final acknowledgment. */
int worker_start(Worker *worker, const char *executable, int runtime);
int worker_tick(const Worker *worker, TickAck *ack);
/* Closes both pipes. The scheduler still owns waitpid() for the child. */
void worker_close(Worker *worker);

/* Worker-side endpoint, used by process.c. Returns 0 on normal exit or EOF
 * on the command pipe, and -1 with errno on protocol or I/O error. */
int worker_serve(int command_fd, int reply_fd, int runtime);

typedef struct {
    int job_id;
    const char *resource_id;
    int capture_time;
    int release_time;
} ResourceUse;

typedef struct HpfState HpfState;

typedef struct {
    int running_blocked;
    int waiter_readied;
    int ready_job_id; /* Present only when waiter_readied is set. */
} HpfEvents;

/* Copies the resource plan. Register each job on arrival before scheduling
 * it. Return NULL/-1 with errno if allocation or input validation fails. */
HpfState *hpf_create(const ResourceUse *uses, size_t count);
void hpf_destroy(HpfState *hpf);
int hpf_add_job(HpfState *hpf, const JobSpec *job);

/* Call once with each newly acknowledged executed time of a running job,
 * including its final tick. Apply returned PCB transitions before deciding
 * whether to preempt. Resource ownership and inheritance live here. */
HpfEvents hpf_on_cpu_tick(HpfState *hpf, int job_id, int executed);
/* Returns -1 for an unregistered job. Smaller values mean higher priority. */
int hpf_effective_priority(const HpfState *hpf, int job_id);

#endif
