"""Linux end-to-end tests. Run sequentially because the clock uses one IPC key."""
import math
from pathlib import Path
import statistics
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = ROOT / "tests" / "examples"


class SchedulerTests(unittest.TestCase):
    def run_jobs(self, selection, workload):
        with tempfile.TemporaryDirectory() as temp:
            cwd = Path(temp)
            for exe in ("process_generator", "scheduler", "clk", "process"):
                name = exe + ".out"
                (cwd / name).symlink_to(ROOT / name)
            (cwd / "processes.txt").write_text(workload)
            result = subprocess.run(["./process_generator.out"], cwd=cwd,
                                    input=selection, capture_output=True,
                                    text=True, timeout=65)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return tuple((cwd / name).read_text() for name in ("scheduler.log", "scheduler.perf"))

    def example(self, name, selection):
        folder = EXAMPLES / name
        log, perf = self.run_jobs(selection, (folder / "processes.txt").read_text())
        self.assertEqual(log, (folder / "expected.log").read_text())
        self.assertEqual(perf, (folder / "expected.perf").read_text())

    def test_pdf_sjf(self):
        self.example("sjf", "1\n")

    def test_pdf_rr_default_switch(self):
        self.example("rr", "2\n3\n\n")

    def check_trace(self, selection, rows, trace):
        # The expected decisions are written out. Derive accounting independently
        # from the intervals between a job's dispatch and its next stop or finish.
        jobs = {pid: (arrival, runtime) for pid, arrival, runtime in rows}
        workload = "".join(f"{p}\t{a}\t{r}\t5\n" for p, a, r in rows)
        actual = self.run_jobs(selection, workload)
        remaining = {p: r for p, _, r in rows}
        wtas, waits, lines = [], [], []
        last_start = {}
        for tick, pid, state in trace:
            arrival, runtime = jobs[pid]
            if state in ("started", "resumed"):
                last_start[pid] = tick
            else:
                remaining[pid] -= tick - last_start.pop(pid)
            wait = tick - arrival - runtime + remaining[pid]
            line = (f"At time {tick} process {pid} {state} arr {arrival} total {runtime}"
                    f" remain {remaining[pid]} wait {wait}")
            if state == "finished":
                self.assertEqual(remaining[pid], 0)
                ta = tick - arrival
                wtas.append(ta / runtime)
                waits.append(wait)
                line += f" TA {ta} WTA {wtas[-1]:.2f}"
            lines.append(line + "\n")
        self.assertEqual(actual[0], "".join(lines))
        utilization = sum(r for _, _, r in rows) * 100 // trace[-1][0] if trace else 0
        mean = statistics.mean(wtas) if wtas else 0
        std = statistics.pstdev(wtas) if wtas else 0
        waiting = math.floor(statistics.mean(waits)) if waits else 0
        self.assertEqual(actual[1], f"CPU utilization = {utilization}%\nAvg WTA = {mean:.2f}\n"
                         f"Avg Waiting = {waiting}\nStd WTA = {std:.2f}\n")

    def test_sjf_ties_nonpreemption_and_switch_end_arrival(self):
        self.check_trace("1\n", [(1,1,4),(7,2,2),(6,2,2),(3,3,2),(9,6,1)], [
            (1,1,"started"), (5,1,"finished"), (6,9,"started"), (7,9,"finished"),
            (8,6,"started"), (10,6,"finished"), (11,7,"started"), (13,7,"finished"),
            (14,3,"started"), (16,3,"finished")])

    def test_sjf_idle_and_arrival_at_finish(self):
        self.check_trace("1\n", [(1,2,1),(2,5,1),(3,6,1)], [
            (2,1,"started"), (3,1,"finished"), (5,2,"started"), (6,2,"finished"),
            (7,3,"started"), (8,3,"finished")])

    def test_rr_arrivals_at_expiry_inside_switch_and_at_end(self):
        self.check_trace("2\n1\n2\n", [(1,1,3),(2,2,1),(3,3,1),(4,4,1)], [
            (1,1,"started"), (2,1,"stopped"), (4,2,"started"), (5,2,"finished"),
            (7,1,"resumed"), (8,1,"stopped"), (10,3,"started"), (11,3,"finished"),
            (13,4,"started"), (14,4,"finished"), (16,1,"resumed"), (17,1,"finished")])

    def test_rr_zero_switch_simultaneous_arrivals_and_exact_finish(self):
        self.check_trace("2\n2\n0\n", [(9,1,1),(4,1,2),(2,1,3)], [
            (1,2,"started"), (3,2,"stopped"), (3,4,"started"), (5,4,"finished"),
            (5,9,"started"), (6,9,"finished"), (6,2,"resumed"), (7,2,"finished")])

    def test_rr_solo_quantum_renewal_then_idle(self):
        self.check_trace("2\n2\n3\n", [(1,1,5),(2,8,1)], [
            (1,1,"started"), (6,1,"finished"), (8,2,"started"), (9,2,"finished")])

    def test_rr_arrival_midway_through_renewed_quantum(self):
        self.check_trace("2\n2\n1\n", [(1,1,6),(2,4,1)], [
            (1,1,"started"), (5,1,"stopped"), (6,2,"started"), (7,2,"finished"),
            (8,1,"resumed"), (10,1,"finished")])

    def test_empty_workloads(self):
        for algorithm in ("1\n", "2\n2\n1\n"):
            with self.subTest(algorithm=algorithm):
                self.check_trace(algorithm, [], [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
