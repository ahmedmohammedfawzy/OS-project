CFLAGS ?= -std=gnu17
HPF_SRC ?= $(wildcard hpf.c)

.PHONY: build clean all run
build:
	$(CC) $(CFLAGS) process_generator.c arrival_ipc.c workload.c -o process_generator.out
	$(CC) $(CFLAGS) clk.c -o clk.out
	$(CC) $(CFLAGS) $(if $(HPF_SRC),-DHAVE_HPF) scheduler.c arrival_ipc.c worker_ipc.c workload.c $(HPF_SRC) -o scheduler.out -lm
	$(CC) $(CFLAGS) process.c worker_ipc.c -o process.out
	$(CC) $(CFLAGS) test_generator.c -o test_generator.out

clean:
	rm -f *.out processes.txt resources.txt scheduler.log scheduler.perf

all: clean build

run:
	./process_generator.out

.PHONY: test
test: build
	python3 tests/test_scheduler.py
