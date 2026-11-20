#define _XOPEN_SOURCE 700
#include "headers.h"
#include "scheduler_api.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t interrupted;

static void request_shutdown(int signal_number)
{
    interrupted = signal_number;
}

static int prompt_number(const char *prompt, int min, int max, int default_value, int *result)
{
    char line[128], *end;
    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin) || (!strchr(line, '\n') && !feof(stdin)))
        return -1;

    char *start = line;
    while (isspace((unsigned char)*start))
        ++start;
    if (!*start && default_value >= min)
    {
        *result = default_value;
        return 0;
    }
    errno = 0;
    long value = strtol(start, &end, 10);
    if (errno || end == start || value < min || value > max)
        return -1;
    while (isspace((unsigned char)*end))
        ++end;
    if (*end)
        return -1;
    *result = (int)value;
    return 0;
}

static int compare_arrivals(const void *left, const void *right)
{
    const JobSpec *a = left, *b = right;
    if (a->arrival != b->arrival)
        return (a->arrival > b->arrival) - (a->arrival < b->arrival);
    return (a->id > b->id) - (a->id < b->id);
}

/* Both inputs are in job ID order at this point. */
static int validate_resources(const char *path, const JobSpec *jobs, size_t count)
{
    ResourceUse *uses;
    size_t use_count;
    if (resource_plan_load(path, &uses, &use_count) == -1)
    {
        perror(path);
        return -1;
    }

    size_t job = 0;
    int valid = 1;
    for (size_t i = 0; i < use_count; ++i)
    {
        while (job < count && jobs[job].id < uses[i].job_id)
            ++job;
        if (job == count || jobs[job].id != uses[i].job_id || uses[i].release_time > jobs[job].runtime)
        {
            fprintf(stderr, "%s: resource %s references an unknown job or exceeds its runtime (job %d)\n",
                    path, uses[i].resource_id, uses[i].job_id);
            valid = 0;
            break;
        }
    }
    resource_plan_free(uses, use_count);
    return valid ? 0 : -1;
}

/* Keep the scheduler and its future workers in their own process group. */
static pid_t launch(char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0)
    {
        if (setpgid(0, 0) == 0)
            execv(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    if (pid > 0 && setpgid(pid, pid) == -1 && errno != EACCES)
    {
        int error = errno;
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
            ;
        errno = error;
        return -1;
    }
    return pid;
}

static void poll_pause(void)
{
    const struct timespec pause = { .tv_nsec = 10000000L };
    nanosleep(&pause, NULL);
}

/* Leave exited children waitable until cleanup so their IDs stay reserved. */
static int child_status(pid_t pid, siginfo_t *status)
{
    *status = (siginfo_t){0};
    if (waitid(P_PID, pid, status, WEXITED | WNOHANG | WNOWAIT) == -1)
        return -1;
    return status->si_pid != 0;
}

static void report_exit(const char *name, const siginfo_t *status)
{
    if (status->si_code == CLD_EXITED)
        fprintf(stderr, "%s exited with status %d.\n", name, status->si_status);
    else
        fprintf(stderr, "%s exited after signal %d.\n", name, status->si_status);
}

static int check_running(pid_t pid, const char *name)
{
    if (interrupted)
        return -1;
    if (pid <= 0)
        return 0;
    siginfo_t status;
    int exited = child_status(pid, &status);
    if (exited == 1)
        report_exit(name, &status);
    else if (exited == -1 && !interrupted)
        perror("waitid");
    return exited == 0 ? 0 : -1;
}

static int wait_for_tick(const volatile int *clock_value, int tick, pid_t clock, pid_t scheduler)
{
    for (;;)
    {
        if (check_running(clock, "Clock") || check_running(scheduler, "Scheduler before INPUT_DONE"))
            return -1;
        if (*clock_value >= tick)
            return 0;
        poll_pause();
    }
}

static int send_to_scheduler(int queue, const ArrivalMessage *message, pid_t clock, pid_t scheduler)
{
    for (;;)
    {
        if (check_running(clock, "Clock") || check_running(scheduler, "Scheduler before INPUT_DONE"))
            return -1;
        if (arrival_try_send(queue, message) == 0)
            return 0;
        if (errno != EAGAIN && errno != EINTR)
        {
            perror("Send arrival message");
            return -1;
        }
        poll_pause();
    }
}

static int deliver_arrivals(int queue, const JobSpec *jobs, size_t count,
                            const volatile int *clock_value, pid_t clock, pid_t scheduler)
{
    size_t next = 0;
    for (int tick = 1; next < count; ++tick)
    {
        if (wait_for_tick(clock_value, tick, clock, scheduler))
            return -1;
        while (next < count && jobs[next].arrival == tick)
        {
            ArrivalMessage message = { .kind = ARRIVAL, .tick = tick, .job = jobs[next++] };
            if (send_to_scheduler(queue, &message, clock, scheduler))
                return -1;
        }
        ArrivalMessage boundary = { .kind = TICK_DONE, .tick = tick };
        if (send_to_scheduler(queue, &boundary, clock, scheduler))
            return -1;
        if (next == count)
            break;
    }
    ArrivalMessage end = { .kind = INPUT_DONE };
    return send_to_scheduler(queue, &end, clock, scheduler);
}

static int wait_for_scheduler(pid_t clock, pid_t scheduler)
{
    while (!interrupted)
    {
        siginfo_t status;
        int exited = child_status(scheduler, &status);
        if (exited == -1)
        {
            if (!interrupted)
                perror("waitid");
            return -1;
        }
        if (exited == 1)
        {
            if (status.si_code == CLD_EXITED && status.si_status == 0)
                return 0;
            report_exit("Scheduler", &status);
            return -1;
        }
        if (check_running(clock, "Clock"))
            return -1;
        poll_pause();
    }
    return -1;
}

static void stop_group(pid_t pid, int graceful)
{
    if (pid <= 0)
        return;
    if (graceful)
    {
        kill(-pid, SIGTERM);
        kill(-pid, SIGCONT);
        for (int i = 0; i < 10; ++i)
        {
            siginfo_t status;
            if (child_status(pid, &status) != 0)
                break;
            poll_pause();
        }
    }
    /* Reclaim any remaining workers, including ones stopped with SIGSTOP. */
    kill(-pid, SIGKILL);
    while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
        ;
}

int main(int argc, char *argv[])
{
    if (argc > 3)
    {
        fprintf(stderr, "Usage: %s [processes.txt [resources.txt]]\n", argv[0]);
        return EXIT_FAILURE;
    }

    struct sigaction action = { .sa_handler = request_shutdown };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 || sigaction(SIGTERM, &action, NULL) == -1)
    {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    const char *jobs_path = argc > 1 ? argv[1] : "processes.txt";
    char *resources_path = argc > 2 ? argv[2] : "resources.txt";
    JobSpec *jobs = NULL;
    size_t count;
    int result = EXIT_FAILURE;
    int queue = -1, clock_segment = -1;
    int *clock_value = NULL;
    pid_t clock = -1, scheduler = -1;

    if (job_list_load(jobs_path, &jobs, &count) == -1)
    {
        perror(jobs_path);
        goto cleanup;
    }

    int algorithm, quantum = 0, switch_ticks = 1;
    if (prompt_number("Algorithm (1 SJF, 2 RR, 3 HPF): ", ALGORITHM_SJF, ALGORITHM_HPF, 0, &algorithm) ||
        (algorithm == ALGORITHM_RR &&
         (prompt_number("RR quantum: ", 1, INT_MAX, 0, &quantum) ||
          prompt_number("RR context-switch ticks [1]: ", 0, INT_MAX, 1, &switch_ticks))))
    {
        if (!interrupted)
            fprintf(stderr, "Invalid scheduling selection or parameter.\n");
        goto cleanup;
    }
    if (algorithm == ALGORITHM_HPF && validate_resources(resources_path, jobs, count))
        goto cleanup;
    if (count > 1)
        qsort(jobs, count, sizeof *jobs, compare_arrivals);
    if (interrupted)
        goto cleanup;

    /* Reserve the supplied clock's key exclusively. clk.out attaches to this
     * segment and changes the sentinel to 0 when it is ready. */
    clock_segment = shmget(SHKEY, 4, IPC_CREAT | IPC_EXCL | 0644);
    if (clock_segment == -1)
    {
        if (errno == EEXIST)
            fprintf(stderr, "Clock key %d is already in use. Only one simulation can run at a time.\n", SHKEY);
        else
            perror("Reserve clock shared memory");
        goto cleanup;
    }
    void *attached = shmat(clock_segment, NULL, 0);
    if (attached == (void *)-1)
    {
        perror("Attach clock shared memory");
        goto cleanup;
    }
    clock_value = attached;
    *clock_value = -1;
    queue = arrival_queue_create();
    if (queue == -1)
    {
        perror("Create arrival queue");
        goto cleanup;
    }

    char *clock_args[] = { "./clk.out", NULL };
    clock = launch(clock_args);
    if (clock == -1)
    {
        perror("Launch clock");
        goto cleanup;
    }
    if (wait_for_tick(clock_value, 0, clock, -1))
        goto cleanup;

    char queue_arg[32], algorithm_arg[32], quantum_arg[32], switch_arg[32];
    snprintf(queue_arg, sizeof queue_arg, "%d", queue);
    snprintf(algorithm_arg, sizeof algorithm_arg, "%d", algorithm);
    snprintf(quantum_arg, sizeof quantum_arg, "%d", quantum);
    snprintf(switch_arg, sizeof switch_arg, "%d", switch_ticks);
    char *scheduler_args[] = { "./scheduler.out", queue_arg, algorithm_arg, quantum_arg,
                              switch_arg, algorithm == ALGORITHM_HPF ? resources_path : "", NULL };
    scheduler = launch(scheduler_args);
    if (scheduler == -1)
    {
        perror("Launch scheduler");
        goto cleanup;
    }
    if (deliver_arrivals(queue, jobs, count, clock_value, clock, scheduler) == 0 &&
        wait_for_scheduler(clock, scheduler) == 0)
        result = EXIT_SUCCESS;

cleanup:
    stop_group(scheduler, result != EXIT_SUCCESS);
    stop_group(clock, 1);
    if (queue >= 0 && arrival_queue_remove(queue) == -1 && errno != EIDRM && errno != EINVAL)
    {
        perror("Remove arrival queue");
        result = EXIT_FAILURE;
    }
    if (clock_value)
        shmdt(clock_value);
    if (clock_segment >= 0 && shmctl(clock_segment, IPC_RMID, NULL) == -1 &&
        errno != EIDRM && errno != EINVAL)
    {
        perror("Remove clock shared memory");
        result = EXIT_FAILURE;
    }
    free(jobs);
    return interrupted ? 128 + interrupted : result;
}
