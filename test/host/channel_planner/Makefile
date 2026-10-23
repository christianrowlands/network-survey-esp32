# Host tests for the channel planner.

ROOT    := ../../..
PLANNER := $(ROOT)/firmware/components/channel_planner
PROTO   := $(ROOT)/firmware/components/ns_proto
BUILD   := ../build/channel_planner
BIN     := $(BUILD)/channel_planner

CC       ?= cc
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O1 -g $(SANITIZE) -I$(PLANNER)/include -I$(PROTO) -I$(PROTO)/nanopb
SRCS     := main.c $(PLANNER)/channel_planner.c

.PHONY: test clean

test: $(BIN)
	$(BIN)

$(BIN): $(SRCS) $(PLANNER)/include/channel_planner.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
