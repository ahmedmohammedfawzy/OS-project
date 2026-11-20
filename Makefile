CFLAGS ?= -std=gnu17

.PHONY: build clean all run
build:
	$(CC) $(CFLAGS) process_generator.c arrival_ipc.c workload.c -o process_generator.out
	$(CC) $(CFLAGS) clk.c -o clk.out
	$(CC) $(CFLAGS) scheduler.c arrival_ipc.c worker_ipc.c workload.c -o scheduler.out -lm
	$(CC) $(CFLAGS) process.c worker_ipc.c -o process.out
	$(CC) $(CFLAGS) test_generator.c -o test_generator.out

clean:
	rm -f *.out processes.txt resources.txt scheduler.log scheduler.perf

all: clean build

run:
	./process_generator.out
