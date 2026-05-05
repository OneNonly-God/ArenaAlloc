# Makefile — Growable Linear Allocator
#
#   make            build optimised test binary
#   make debug      build with sanitisers (ASan + UBSan)
#   make run        build and run tests
#   make clean      remove build artifacts

CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic \
           -Wcast-align -Wshadow -Wstrict-prototypes \
           -pthread
OPT     = -O2
DBGFLAGS = -O1 -g3 -fsanitize=address,undefined \
            -fno-omit-frame-pointer

TARGET  = arena_test
SRCS    = arena.c arena_test.c

.PHONY: all debug run clean

all: $(TARGET)

$(TARGET): $(SRCS) arena.h
	$(CC) $(CFLAGS) $(OPT) -o $@ $(SRCS)
	@echo "Built $(TARGET) (optimised)"

debug: $(SRCS) arena.h
	$(CC) $(CFLAGS) $(DBGFLAGS) -o $(TARGET)_debug $(SRCS)
	@echo "Built $(TARGET)_debug (ASan + UBSan)"

run: all
	./$(TARGET)

clean:
	rm -f $(TARGET) $(TARGET)_debug
