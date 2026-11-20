#define _POSIX_C_SOURCE 200809L
#include "scheduler_api.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *text, int *value)
{
    char *end;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || end == text || *end || parsed < INT_MIN || parsed > INT_MAX)
        return -1;
    *value = (int)parsed;
    return 0;
}

static int compare_int(int a, int b)
{
    return (a > b) - (a < b);
}

static int compare_jobs(const void *left, const void *right)
{
    const JobSpec *a = left, *b = right;
    return compare_int(a->id, b->id);
}

static int compare_uses(const void *left, const void *right)
{
    const ResourceUse *a = left, *b = right;
    int order = compare_int(a->job_id, b->job_id);
    return order ? order : compare_int(a->capture_time, b->capture_time);
}

static int parse_job(char *const fields[], void *row)
{
    JobSpec *job = row;
    if (number(fields[0], &job->id) || number(fields[1], &job->arrival) ||
        number(fields[2], &job->runtime) || number(fields[3], &job->priority) ||
        job->arrival < 1 || job->runtime < 1 || job->priority < 0 || job->priority > 10)
        return EINVAL;
    return 0;
}

static int parse_use(char *const fields[], void *row)
{
    ResourceUse *use = row;
    if (number(fields[0], &use->job_id) || number(fields[2], &use->capture_time) ||
        number(fields[3], &use->release_time) || use->capture_time < 1 ||
        use->release_time <= use->capture_time)
        return EINVAL;
    use->resource_id = strdup(fields[1]);
    return use->resource_id ? 0 : ENOMEM;
}

static void free_use(void *row)
{
    ResourceUse *use = row;
    free((void *)use->resource_id);
}

static int load_rows(const char *path, size_t row_size, int (*parse)(char *const [], void *),
                     void (*free_row)(void *), void **result, size_t *result_count)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;
    char *line = NULL;
    unsigned char *rows = NULL;
    size_t line_capacity = 0, count = 0, capacity = 0, line_number = 0;
    int error = 0;

    while (getline(&line, &line_capacity, file) != -1)
    {
        ++line_number;
        char *start = line;
        while (isspace((unsigned char)*start))
            ++start;
        if (!*start || *start == '#')
            continue;

        char *fields[4], *rest;
        size_t n = 0;
        char *field = strtok_r(start, " \t\r\n\v\f", &rest);
        while (field && n < 4)
        {
            fields[n++] = field;
            field = strtok_r(NULL, " \t\r\n\v\f", &rest);
        }
        if (n != 4 || field)
        {
            error = EINVAL;
            break;
        }
        if (count == capacity)
        {
            if (capacity > SIZE_MAX / row_size / 2)
            {
                error = ENOMEM;
                break;
            }
            size_t next_capacity = capacity ? capacity * 2 : 16;
            void *extended = realloc(rows, row_size * next_capacity);
            if (!extended)
            {
                error = errno;
                break;
            }
            rows = extended;
            capacity = next_capacity;
        }
        error = parse(fields, rows + row_size * count);
        if (error)
            break;
        ++count;
    }

    if (!error && !feof(file))
        error = errno ? errno : EIO;
    if (error == EINVAL)
        fprintf(stderr, "%s:%zu: malformed input row\n", path, line_number);
    free(line);
    fclose(file);
    if (error)
    {
        if (free_row)
            for (size_t i = 0; i < count; ++i)
                free_row(rows + row_size * i);
        free(rows);
        errno = error;
        return -1;
    }
    *result = rows;
    *result_count = count;
    return 0;
}

int job_list_load(const char *path, JobSpec **jobs, size_t *count)
{
    void *rows;
    size_t length;
    if (load_rows(path, sizeof(JobSpec), parse_job, NULL, &rows, &length) == -1)
        return -1;
    JobSpec *list = rows;
    if (length > 1)
        qsort(list, length, sizeof *list, compare_jobs);
    for (size_t i = 1; i < length; ++i)
    {
        if (list[i - 1].id == list[i].id)
        {
            fprintf(stderr, "%s: duplicate job ID %d\n", path, list[i].id);
            free(list);
            errno = EINVAL;
            return -1;
        }
    }
    *jobs = list;
    *count = length;
    return 0;
}

int resource_plan_load(const char *path, ResourceUse **uses, size_t *count)
{
    void *rows;
    size_t length;
    if (load_rows(path, sizeof(ResourceUse), parse_use, free_use, &rows, &length) == -1)
        return -1;
    ResourceUse *list = rows;
    if (length > 1)
        qsort(list, length, sizeof *list, compare_uses);
    for (size_t i = 1; i < length; ++i)
    {
        if (list[i - 1].job_id == list[i].job_id && list[i - 1].release_time > list[i].capture_time)
        {
            fprintf(stderr, "%s: overlapping resource intervals for job %d\n", path, list[i].job_id);
            resource_plan_free(list, length);
            errno = EINVAL;
            return -1;
        }
    }
    *uses = list;
    *count = length;
    return 0;
}

void resource_plan_free(ResourceUse *uses, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        free_use(&uses[i]);
    free(uses);
}
