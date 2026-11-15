# CampusCompute - build file
# Usage:
#   make            build ./campuscompute
#   make test       run every feature once (non-interactive smoke test)
#   make clean      remove object files and the executable
#   make distclean  also remove generated logs
#   make zip SRN=PES1UGxxxxxxx   create the submission archive SRN.zip

CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS = -Iinclude
LDLIBS   = -pthread -lrt

TARGET  = campuscompute
SRCS    = $(wildcard src/*.c)
OBJS    = $(SRCS:src/%.c=build/%.o)

.PHONY: all clean distclean test zip

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c include/campus.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -c $< -o $@

build:
	mkdir -p build

test: $(TARGET)
	./$(TARGET) help
	./$(TARGET) compare tests/jobs_sample.txt 2
	./$(TARGET) logger 3 5 4
	./$(TARGET) ipc tests/jobs_sample.txt
	./$(TARGET) banker tests/banker_safe.txt
	./$(TARGET) banker tests/banker_unsafe.txt
	./$(TARGET) workers normal 3
	./$(TARGET) workers fail all
	./$(TARGET) rlimit all
	-./$(TARGET) jobs tests/jobs_invalid.txt
	./$(TARGET) cleanup-check

clean:
	rm -rf build $(TARGET)

distclean: clean
	rm -rf logs

zip: clean
	@if [ -z "$(SRN)" ]; then echo "usage: make zip SRN=<your SRN>"; exit 2; fi
	rm -f $(SRN).zip
	zip -r $(SRN).zip Makefile README.md include src tests
	unzip -l $(SRN).zip
	unzip -t $(SRN).zip
