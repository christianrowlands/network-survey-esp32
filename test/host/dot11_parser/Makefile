# Host tests for the 802.11 parser. Built with AddressSanitizer and
# UndefinedBehaviorSanitizer so out-of-bounds reads fail the run.

ROOT   := ../../..
PARSER := $(ROOT)/firmware/components/dot11_parser
BUILD  := ../build/dot11_parser
BIN    := $(BUILD)/dot11_parser

CC       ?= cc
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CFLAGS   := -std=c99 -Wall -Wextra -Werror -O1 -g $(SANITIZE) -I$(PARSER)/include
SRCS     := main.c $(PARSER)/dot11_parser.c $(PARSER)/dot11_ies.c

.PHONY: test clean

test: $(BIN)
	$(BIN)

$(BIN): $(SRCS) $(PARSER)/include/dot11_parser.h ../common/frame_builder.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
