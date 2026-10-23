# Host tests for the sniffer's raw frame helpers.

ROOT   := ../../..
SNIFF  := $(ROOT)/firmware/components/sniffer
BUILD  := ../build/raw
BIN    := $(BUILD)/raw

CC       ?= cc
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O1 -g $(SANITIZE) -I$(SNIFF)
SRCS     := main.c $(SNIFF)/raw_frame.c
HDRS     := $(SNIFF)/raw_frame.h

.PHONY: test clean

test: $(BIN)
	$(BIN)

$(BIN): $(SRCS) $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
