CFLAGS ?= -std=gnu17

.PHONY: build clean all run
build:
	$(CC) $(CFLAGS) process_generator.c arrival_ipc.c workload.c -o process_generator.out
	$(CC) $(CFLAGS) clk.c -o clk.out
	$(CC) $(CFLAGS) scheduler.c arrival_ipc.c worker_ipc.c workload.c hpf.c -o scheduler.out -lm
	$(CC) $(CFLAGS) process.c worker_ipc.c -o process.out
	$(CC) $(CFLAGS) test_generator.c -o test_generator.out

clean:
	rm -f *.out processes.txt resources.txt scheduler.log scheduler.perf

all: clean build

run:
	./process_generator.out

.PHONY: test
test: build
	@set -eu; \
	root=$$(pwd); work=$$(mktemp -d); \
	trap 'rm -rf -- "$$work"' EXIT; \
	for exe in process_generator scheduler clk process; do \
		ln -s "$$root/$$exe.out" "$$work/$$exe.out"; \
	done; \
	cd "$$work"; \
	for run in sjf:1 rr:2 hpf:3 hpf:4 hpf-aging:3 hpf-aging:4; do \
		example=$${run%:*}; algorithm=$${run#*:}; \
		fixture="$$root/tests/examples/$$example"; \
		expected=expected; \
		if [ "$$run" = hpf-aging:3 ]; then expected=expected-plain; fi; \
		printf 'Testing %s with algorithm %s\n' "$$example" "$$algorithm"; \
		rm -f scheduler.log scheduler.perf; \
		case "$$algorithm" in \
			2) printf '2\n3\n\n';; \
			*) printf '%s\n' "$$algorithm";; \
		esac | timeout -k 5s 65s ./process_generator.out "$$fixture/processes.txt" "$$fixture/resources.txt"; \
		diff -u --strip-trailing-cr "$$fixture/$$expected.log" scheduler.log; \
		diff -u --strip-trailing-cr "$$fixture/$$expected.perf" scheduler.perf; \
	done; \
	printf 'All scheduler example runs passed.\n'
