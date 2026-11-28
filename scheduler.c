#include "headers.h"
#include "scheduler_api.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <time.h>

/* Queue membership encodes a PCB's state. A ready job with no PID has
 * never run. Finished PCBs are reclaimed after the child's final ACK. */
typedef struct PCB {
    JobSpec job;
    Worker worker;
    int remaining;
    int ready_since;
    struct PCB *next;
} PCB;

typedef struct {
    PCB *head, *tail;
} Queue;

typedef struct {
    FILE *log, *perf;
    long long busy, total_wait;
    int finished, last_finish;
    double mean, m2;
} Report;

typedef struct {
    Queue ready, blocked;
    PCB *running;
    HpfState *hpf;
    Report report;
    int algorithm, quantum, delay, slice, tick, queue, input_done;
    long long dispatch_at;
} Scheduler;

static int protocol_error(void)
{
    errno = EPROTO;
    return -1;
}

static void append(Queue *queue, PCB *p)
{
    p->next = NULL;
    if (queue->tail)
        queue->tail->next = p;
    else
        queue->head = p;
    queue->tail = p;
}

static void make_ready(Scheduler *s, PCB *p)
{
    p->ready_since = s->tick;
    append(&s->ready, p);
}

static PCB *remove_after(Queue *queue, PCB *prev)
{
    PCB *p = prev ? prev->next : queue->head;
    if (prev)
        prev->next = p->next;
    else
        queue->head = p->next;
    if (!p->next)
        queue->tail = prev;
    p->next = NULL;
    return p;
}

static int ready_priority(const Scheduler *s, const PCB *p)
{
    int priority = hpf_effective_priority(s->hpf, p->job.id);
    if (s->algorithm != ALGORITHM_HPF_AGING)
        return priority;

    /* Switch time counts automatically while the job stays in Ready. */
    int waited = s->tick - p->ready_since;
    if (waited < 20)
        return priority;
    int aged = p->job.priority - (1 + (waited - 20) / 5);
    if (aged < 0)
        aged = 0;
    return aged < priority ? aged : priority;
}

/* RR takes the head in O(1). SJF scans only at dispatch. HPF checks live
 * ready priorities because both resource events and aging can change them. */
static int preferred(const Scheduler *s, const PCB *a, const PCB *b)
{
    if (s->hpf) {
        int pa = ready_priority(s, a);
        int pb = ready_priority(s, b);
        return pa == pb ? a->job.id < b->job.id : pa < pb;
    }
    if (a->job.runtime != b->job.runtime)
        return a->job.runtime < b->job.runtime;
    if (a->job.arrival != b->job.arrival)
        return a->job.arrival < b->job.arrival;
    return a->job.id < b->job.id;
}

static PCB *best_predecessor(const Scheduler *s)
{
    PCB *best = s->ready.head, *prev_best = NULL;
    if (s->algorithm != ALGORITHM_RR)
        for (PCB *prev = best; prev->next; prev = prev->next)
            if (preferred(s, prev->next, best)) {
                best = prev->next;
                prev_best = prev;
            }
    return prev_best;
}

static int log_event(Report *r, int t, const PCB *p, const char *state)
{
    int ta = t - p->job.arrival;
    int wait = ta - (p->job.runtime - p->remaining);
    if (fprintf(r->log, "At time %d process %d %s arr %d total %d remain %d wait %d",
                t, p->job.id, state, p->job.arrival, p->job.runtime, p->remaining, wait) < 0)
        return -1;
    if (p->remaining == 0) {
        double wta = (double)ta / p->job.runtime;
        if (fprintf(r->log, " TA %d WTA %.2f", ta, wta) < 0)
            return -1;
        ++r->finished;
        r->last_finish = t;
        r->busy += p->job.runtime;
        r->total_wait += wait;
        double delta = wta - r->mean;
        r->mean += delta / r->finished;
        r->m2 += delta * (wta - r->mean);
    }
    return fputc('\n', r->log) == EOF ? -1 : 0;
}

static int write_perf(Report *r)
{
    long long utilization = r->finished ? 100 * r->busy / r->last_finish : 0;
    long long wait = r->finished ? r->total_wait / r->finished : 0;
    double deviation = r->finished ? sqrt(r->m2 / r->finished) : 0;
    return fprintf(r->perf, "CPU utilization = %lld%%\nAvg WTA = %.2f\n"
                   "Avg Waiting = %lld\nStd WTA = %.2f\n",
                   utilization, r->mean, wait, deviation) < 0 ? -1 : 0;
}

static int wait_child(pid_t pid, int *status, int options)
{
    pid_t result;
    do {
        result = waitpid(pid, status, options);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? -1 : 0;
}

static int suspend_job(Scheduler *s, Queue *destination)
{
    PCB *p = s->running;
    int status;
    if (kill(p->worker.pid, SIGSTOP) || wait_child(p->worker.pid, &status, WUNTRACED))
        return -1;
    if (!WIFSTOPPED(status))
        return protocol_error();
    if (log_event(&s->report, s->tick, p, "stopped"))
        return -1;
    if (destination == &s->ready)
        make_ready(s, p);
    else
        append(destination, p);
    s->running = NULL;
    return 0;
}

static int account_interval(Scheduler *s)
{
    PCB *p = s->running;
    TickAck ack;
    if (worker_tick(&p->worker, &ack))
        return -1;
    if (ack.remaining != p->remaining - 1 || ack.finished != (ack.remaining == 0))
        return protocol_error();
    p->remaining = ack.remaining;
    ++s->slice;
    if (s->hpf) {
        HpfEvents events = hpf_on_cpu_tick(s->hpf, p->job.id, p->job.runtime - p->remaining);
        if (events.waiter_readied) {
            PCB *prev = NULL, *w = s->blocked.head;
            while (w && w->job.id != events.ready_job_id) {
                prev = w;
                w = w->next;
            }
            if (!w)
                return protocol_error();
            make_ready(s, remove_after(&s->blocked, prev));
        }
        if (events.running_blocked)
            return ack.finished ? protocol_error() : suspend_job(s, &s->blocked);
    }
    if (ack.finished) {
        int status;
        worker_close(&p->worker);
        if (wait_child(p->worker.pid, &status, 0))
            return -1;
        p->worker.pid = 0;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return protocol_error();
        if (log_event(&s->report, s->tick, p, "finished"))
            return -1;
        free(p);
        s->running = NULL;
    }
    return 0;
}

static int receive_batch(Scheduler *s)
{
    while (!s->input_done) {
        ArrivalMessage msg;
        if (arrival_receive(s->queue, &msg)) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (msg.kind == INPUT_DONE) {
            s->input_done = 1;
            break;
        }
        if (msg.tick != s->tick)
            return protocol_error();
        if (msg.kind == TICK_DONE)
            break;
        if (msg.job.arrival != s->tick || msg.job.runtime <= 0)
            return protocol_error();
        PCB *p = calloc(1, sizeof *p);
        if (!p)
            return -1;
        p->job = msg.job;
        p->remaining = msg.job.runtime;
        make_ready(s, p);
        if (s->hpf && hpf_add_job(s->hpf, &p->job))
            return -1;
    }
    return 0;
}

static int should_stop(Scheduler *s)
{
    if (s->algorithm == ALGORITHM_RR && s->slice == s->quantum) {
        s->slice = 0;
        return s->ready.head != NULL;
    }
    if (s->hpf && s->ready.head) {
        PCB *prev = best_predecessor(s);
        PCB *best = prev ? prev->next : s->ready.head;
        return ready_priority(s, best) <
               hpf_effective_priority(s->hpf, s->running->job.id);
    }
    return 0;
}

static int dispatch(Scheduler *s)
{
    PCB *p = remove_after(&s->ready, best_predecessor(s));
    p->ready_since = 0;
    s->running = p;
    s->slice = 0;
    int resume = p->worker.pid > 0;
    if (resume) {
        if (kill(p->worker.pid, SIGCONT))
            return -1;
    } else if (worker_start(&p->worker, "./process.out", p->job.runtime)) {
        return -1;
    }
    return log_event(&s->report, s->tick, p, resume ? "resumed" : "started");
}

static int simulate(Scheduler *s)
{
    for (s->tick = 1; ; ++s->tick) {
        while (*(volatile int *)shmaddr < s->tick)
            nanosleep(&(struct timespec){ .tv_nsec = 1000000 }, NULL);
        /* First settle [tick-1, tick), then admit this tick's whole batch.
         * Only now may an expired RR job go to the back of the queue. */
        int was_running = s->running != NULL;
        if (was_running && account_interval(s))
            return -1;
        if (receive_batch(s))
            return -1;
        if (s->running && should_stop(s) && suspend_job(s, &s->ready))
            return -1;
        if (!s->running && s->ready.head) {
            /* Charge once on departure. Select at the end of the delay
             * so arrivals throughout the switch participate in dispatch. */
            if (was_running)
                s->dispatch_at = (long long)s->tick + s->delay;
            if (s->tick >= s->dispatch_at && dispatch(s))
                return -1;
        }
        if (s->input_done && !s->running && !s->ready.head)
            return s->blocked.head ? protocol_error() : write_perf(&s->report);
        if (s->tick == INT_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
    }
}

static void dispose(PCB *p)
{
    while (p) {
        PCB *next = p->next;
        if (p->worker.pid > 0) {
            worker_close(&p->worker);
            kill(p->worker.pid, SIGCONT); /* EOF lets interrupted workers exit. */
            wait_child(p->worker.pid, NULL, 0);
        }
        free(p);
        p = next;
    }
}

static int number(const char *text)
{
    char *end;
    errno = 0;
    long n = strtol(text, &end, 10);
    return errno || end == text || *end || n < 0 || n > INT_MAX ? -1 : (int)n;
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "Usage: scheduler.out QUEUE_ID ALGORITHM QUANTUM SWITCH_TICKS RESOURCES_PATH\n");
        return EXIT_FAILURE;
    }
    Scheduler s = { .queue = number(argv[1]), .algorithm = number(argv[2]),
                    .quantum = number(argv[3]), .delay = number(argv[4]) };
    if (s.queue < 0 || s.algorithm < ALGORITHM_SJF || s.algorithm > ALGORITHM_HPF_AGING ||
        (s.algorithm == ALGORITHM_RR ? s.quantum < 1 || s.delay < 0 : s.quantum != 0 || s.delay != 1)) {
        fprintf(stderr, "Invalid scheduler parameters\n");
        return EXIT_FAILURE;
    }
    if (s.algorithm == ALGORITHM_HPF || s.algorithm == ALGORITHM_HPF_AGING) {
        ResourceUse *uses;
        size_t count;
        if (resource_plan_load(argv[5], &uses, &count)) {
            perror(argv[5]);
            return EXIT_FAILURE;
        }
        s.hpf = hpf_create(uses, count);
        resource_plan_free(uses, count);
        if (!s.hpf) {
            perror("Create HPF policy");
            return EXIT_FAILURE;
        }
    }
    initClk();
    s.report.log = fopen("scheduler.log", "w");
    s.report.perf = fopen("scheduler.perf", "w");
    int failed = shmaddr == (void *)-1 || !s.report.log || !s.report.perf ||
                 signal(SIGPIPE, SIG_IGN) == SIG_ERR || simulate(&s);
    if (failed)
        perror("Scheduler");
    dispose(s.running);
    dispose(s.ready.head);
    dispose(s.blocked.head);
    if (s.hpf)
        hpf_destroy(s.hpf);
    if (s.report.log && fclose(s.report.log))
        failed = 1;
    if (s.report.perf && fclose(s.report.perf))
        failed = 1;
    if (shmaddr != (void *)-1)
        destroyClk(false);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
