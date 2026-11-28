#include "scheduler_api.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct HpfJob HpfJob;

typedef struct {
    HpfJob *owner;
    HpfJob *waiters;
} Resource;

typedef struct {
    int job_id;
    int capture;
    int release;
    Resource *resource;
} PlannedUse;

struct HpfJob {
    int id;
    int priority;
    int runtime;
    size_t next_use;
    size_t end_use;
    const PlannedUse *held;
    const PlannedUse *pending;
    HpfJob *next_waiter;
};

struct HpfState {
    Resource *resources;
    PlannedUse *uses;
    size_t use_count;
    /* Stable job allocations, indexed by ID in this sorted pointer array. */
    HpfJob **jobs;
    size_t job_count;
    size_t job_capacity;
};

static int compare_resource_names(const void *left, const void *right)
{
    const ResourceUse *const *a = left;
    const ResourceUse *const *b = right;
    return strcmp((*a)->resource_id, (*b)->resource_id);
}

static int compare_planned_uses(const void *left, const void *right)
{
    const PlannedUse *a = left, *b = right;
    if (a->job_id != b->job_id)
        return (a->job_id > b->job_id) - (a->job_id < b->job_id);
    return (a->capture > b->capture) - (a->capture < b->capture);
}

/* Resolve names into shared Resource objects. The input strings are only
 * needed during setup; the scheduler can free the input plan afterward. */
static int copy_plan(HpfState *hpf, const ResourceUse *uses, size_t count)
{
    const ResourceUse **ordered = calloc(count, sizeof *ordered);
    if (!ordered)
        return -1;
    for (size_t i = 0; i < count; ++i) {
        if (!uses[i].resource_id || !*uses[i].resource_id || uses[i].capture_time < 1 ||
            uses[i].release_time <= uses[i].capture_time) {
            free(ordered);
            errno = EINVAL;
            return -1;
        }
        ordered[i] = &uses[i];
    }
    qsort(ordered, count, sizeof *ordered, compare_resource_names);
    Resource *resource = hpf->resources;
    for (size_t i = 0; i < count; ++i) {
        const ResourceUse *use = ordered[i];
        if (i && strcmp(ordered[i - 1]->resource_id, use->resource_id))
            ++resource;
        hpf->uses[i] = (PlannedUse){use->job_id, use->capture_time, use->release_time, resource};
    }
    free(ordered);

    qsort(hpf->uses, count, sizeof *hpf->uses, compare_planned_uses);
    for (size_t i = 1; i < count; ++i) {
        const PlannedUse *previous = &hpf->uses[i - 1], *current = &hpf->uses[i];
        if (previous->job_id == current->job_id && previous->release > current->capture) {
            errno = EINVAL;
            return -1;
        }
    }
    hpf->use_count = count;
    return 0;
}

HpfState *hpf_create(const ResourceUse *uses, size_t count)
{
    if (count && !uses) {
        errno = EINVAL;
        return NULL;
    }
    HpfState *hpf = calloc(1, sizeof *hpf);
    if (!hpf || !count)
        return hpf;

    hpf->uses = calloc(count, sizeof *hpf->uses);
    hpf->resources = calloc(count, sizeof *hpf->resources);
    if (!hpf->uses || !hpf->resources || copy_plan(hpf, uses, count)) {
        int error = errno;
        hpf_destroy(hpf);
        errno = error;
        return NULL;
    }
    return hpf;
}

void hpf_destroy(HpfState *hpf)
{
    if (!hpf)
        return;
    for (size_t i = 0; i < hpf->job_count; ++i)
        free(hpf->jobs[i]);
    free(hpf->jobs);
    free(hpf->uses);
    free(hpf->resources);
    free(hpf);
}

static size_t job_position(const HpfState *hpf, int id)
{
    size_t first = 0, end = hpf->job_count;
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        if (hpf->jobs[middle]->id < id)
            first = middle + 1;
        else
            end = middle;
    }
    return first;
}

static HpfJob *job_at(const HpfState *hpf, size_t position, int id)
{
    return position < hpf->job_count && hpf->jobs[position]->id == id ? hpf->jobs[position] : NULL;
}

int hpf_add_job(HpfState *hpf, const JobSpec *spec)
{
    size_t position = job_position(hpf, spec->id);
    if (job_at(hpf, position, spec->id) || spec->priority < 0 || spec->priority > 10 || spec->runtime < 1) {
        errno = EINVAL;
        return -1;
    }

    size_t first = 0, end = hpf->use_count;
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        if (hpf->uses[middle].job_id < spec->id)
            first = middle + 1;
        else
            end = middle;
    }
    while (end < hpf->use_count && hpf->uses[end].job_id == spec->id) {
        if (hpf->uses[end].release > spec->runtime) {
            errno = EINVAL;
            return -1;
        }
        ++end;
    }

    if (hpf->job_count == hpf->job_capacity) {
        if (hpf->job_capacity > SIZE_MAX / sizeof *hpf->jobs / 2) {
            errno = ENOMEM;
            return -1;
        }
        size_t capacity = hpf->job_capacity ? 2 * hpf->job_capacity : 8;
        HpfJob **jobs = realloc(hpf->jobs, capacity * sizeof *jobs);
        if (!jobs)
            return -1;
        hpf->jobs = jobs;
        hpf->job_capacity = capacity;
    }
    HpfJob *job = malloc(sizeof *job);
    if (!job)
        return -1;
    *job = (HpfJob){ .id = spec->id, .priority = spec->priority, .runtime = spec->runtime,
                     .next_use = first, .end_use = end };
    memmove(hpf->jobs + position + 1, hpf->jobs + position,
            (hpf->job_count - position) * sizeof *hpf->jobs);
    hpf->jobs[position] = job;
    ++hpf->job_count;
    return 0;
}

static void enqueue_waiter(Resource *resource, HpfJob *job)
{
    HpfJob **link = &resource->waiters;
    while (*link && ((*link)->priority < job->priority ||
           ((*link)->priority == job->priority && (*link)->id < job->id)))
        link = &(*link)->next_waiter;
    job->next_waiter = *link;
    *link = job;
}

static void release_resource(HpfJob *holder, HpfEvents *events)
{
    Resource *resource = holder->held->resource;
    holder->held = NULL; /* The holder immediately loses inherited priority. */
    HpfJob *next = resource->waiters;
    resource->owner = next;
    if (!next)
        return;

    resource->waiters = next->next_waiter;
    next->next_waiter = NULL;
    next->held = next->pending;
    next->pending = NULL;
    events->waiter_readied = 1;
    events->ready_job_id = next->id;
}

HpfEvents hpf_on_cpu_tick(HpfState *hpf, int job_id, int executed)
{
    HpfEvents events = {0};
    size_t position = job_position(hpf, job_id);
    HpfJob *job = job_at(hpf, position, job_id);
    if (!job || job->pending)
        return events;

    if (job->held && job->held->release == executed)
        release_resource(job, &events);

    /* A valid interval may release at the final tick but cannot capture there. */
    if (executed == job->runtime) {
        free(job);
        --hpf->job_count;
        memmove(hpf->jobs + position, hpf->jobs + position + 1,
                (hpf->job_count - position) * sizeof *hpf->jobs);
        return events;
    }

    if (job->next_use < job->end_use && hpf->uses[job->next_use].capture == executed) {
        const PlannedUse *use = &hpf->uses[job->next_use++];
        Resource *resource = use->resource;
        if (resource->owner) {
            job->pending = use;
            enqueue_waiter(resource, job);
            events.running_blocked = 1;
        } else {
            resource->owner = job;
            job->held = use;
        }
    }
    return events;
}

int hpf_effective_priority(const HpfState *hpf, int job_id)
{
    const HpfJob *job = job_at(hpf, job_position(hpf, job_id), job_id);
    if (!job)
        return -1;
    const HpfJob *highest_waiter = job->held ? job->held->resource->waiters : NULL;
    return highest_waiter && highest_waiter->priority < job->priority ? highest_waiter->priority : job->priority;
}
