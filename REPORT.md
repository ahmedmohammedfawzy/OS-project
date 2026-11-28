# Phase 1 CPU Scheduler Report Draft

The simulator schedules one CPU under non-preemptive Shortest Job First (SJF), Round Robin (RR), and preemptive Highest Priority First (HPF) with priority inheritance. The bonus mode adds ready queue aging and demotion to HPF. It runs on Linux using the supplied clock and emits `scheduler.log` and `scheduler.perf` for each run.

## Process design and data structures

The generator reads jobs into an array sorted by arrival tick and job ID. It sends a batch through a System V message queue at each tick and marks the end of that batch so the scheduler can consider every same-tick arrival before dispatching. The clock publishes the current tick in shared memory. A worker receives one pipe command for each CPU interval it was assigned and replies with its remaining runtime and completion status. The generator owns the clock and arrival queue and releases those resources at shutdown.

The scheduler keeps a process control block (PCB) for each live job. It stores the job's input fields, acknowledged remaining runtime, and its worker's PID and pipe descriptors. Linked queues hold ready and blocked PCBs. The scheduler accounts for the preceding CPU interval, applies resource and completion events, admits the next arrival batch, then checks for preemption or dispatch. A context switch consumes time without sending any worker a CPU command.

HPF stores resource uses sorted by job ID and capture time and keeps an ID index of live jobs. Each resource has one owner and a linked waiter list ordered by base priority and job ID. The first waiter identifies both the next owner and the highest priority that the current owner may inherit. A job advances through its resource plan using its acknowledged executed time.

RR inserts and removes jobs at the ready queue ends in constant time. SJF scans the ready queue at dispatch. HPF scans the ready queue when choosing a job and uses binary search to look up each effective priority. Inserting a waiter scans one resource's waiter list. Transferring the resource removes the first waiter in constant time.

## Algorithms

| Algorithm | Selection and preemption |
|---|---|
| SJF | Select the ready job with the smallest total runtime. Break ties by arrival, then job ID. A running job continues until completion. |
| RR | Remove jobs from the head of a FIFO queue. At quantum expiry, append an unfinished job behind that tick's arrivals if another job is ready. A lone running job renews its quantum. |
| HPF | Select the smallest effective priority value, breaking ready-queue ties by ID. Preempt a running job only for a strictly higher priority. A resource owner temporarily inherits the priority of a higher-priority waiter and loses that boost at release. |
| HPF with aging and demotion | Improve ready priority by one after 20 consecutive ready ticks and again every five ticks, down to priority zero. Use the better of the aged and inherited priorities. Dispatch resets aging while resource inheritance continues to apply. |

In HPF, capture and release use a job's CPU time. An unavailable resource blocks the requester. Release transfers the resource directly to the highest-priority waiter and makes it ready. When release and capture coincide, the release is processed first.

Algorithm 4 records when each job enters the ready queue. The difference between the current tick and that timestamp includes context-switch time and excludes any earlier blocked time. Aging can trigger preemption without a new arrival. This consecutive wait differs from the cumulative waiting time in the log.

## Results

The following results are for the supplied examples. Each fixture is under `tests/examples`. The SJF and RR examples use different workloads. The last four rows compare Algorithms 3 and 4 on the same two HPF workloads.

| Example | CPU utilization | Average WTA | Average wait | Population standard deviation of WTA |
|---|---:|---:|---:|---:|
| SJF | 78% | 3.06 | 6 | 1.20 |
| RR, quantum 3 and switch delay 1 | 68% | 5.59 | 10 | 3.30 |
| Resource workload, HPF (3) | 65% | 3.31 | 10 | 0.32 |
| Resource workload, HPF with aging (4) | 65% | 3.31 | 10 | 0.32 |
| Aging workload, HPF (3) | 90% | 3.11 | 13 | 2.63 |
| Aging workload, HPF with aging (4) | 88% | 3.20 | 15 | 2.69 |

On the resource workload, no ready job waited for 20 consecutive ticks, so both modes produced the same log. On the aging workload, P2's first dispatch moved from tick 37 under plain HPF to tick 23 with aging. Its wait **before first getting the CPU** fell from 35 to 21 ticks. Dispatch reset P2's priority, and P3 preempted it at tick 25. P2 then spent another 15 ticks waiting to resume, so its **total waiting time** rose from 35 to 36 ticks and its finish moved from tick 43 to tick 44.

P1 also waited three ticks longer, while P3's wait stayed the same. The longer waits raised both average waiting time and average WTA. Aging used four context switches instead of three to complete the same 39 ticks of CPU work, lowering reported utilization from 90% to 88%. In this workload, aging reduced the longest wait for a first turn on the CPU, while the jobs' overall completion metrics worsened.

`make test` compares every run's complete scheduler log and performance report with its expected files.

## TA-approved assumptions

<!-- Before submission, list any additional assumptions approved by the TA, or confirm that there were none. -->

## Workload distribution and actual time

<!-- Before submission, confirm the actual contributors and record time spent. -->

| Workstream | Work | Contributor | Actual time |
|---|---|---|---|
| Person 1 | Input parsing, generator, worker and IPC | TBD | TBD |
| Person 2 | Scheduler loop, SJF, RR, logging and metrics | TBD | TBD |
| Person 3 | HPF priority inheritance and resource handling | TBD | TBD |
| Bonus | HPF aging, demotion, and policy comparison | TBD | TBD |
| Integration | End-to-end tests and report | TBD | TBD |
