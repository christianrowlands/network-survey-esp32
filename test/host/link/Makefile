# Host tests for the link layer (framing, ring buffer). No ESP-IDF needed.
#   make            build and check against test/fixtures/framing
#   make fixtures   rewrite the framing fixtures (after a deliberate change)

ROOT     := ../../..
LINK     := $(ROOT)/firmware/components/link
PROTO    := $(ROOT)/firmware/components/ns_proto
FIXTURES := $(ROOT)/test/fixtures
BUILD    := ../build/link
BIN      := $(BUILD)/link_test

CC     ?= cc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -I$(LINK) -I$(PROTO) -I$(PROTO)/nanopb
SRCS   := main.c $(LINK)/link_frame.c $(LINK)/link_ring.c $(PROTO)/ns_board.pb.c \
          $(PROTO)/ns_proto_fit.c $(PROTO)/nanopb/pb_common.c $(PROTO)/nanopb/pb_encode.c \
          $(PROTO)/nanopb/pb_decode.c
HDRS   := $(LINK)/link_frame.h $(LINK)/link_ring.h $(PROTO)/ns_board.pb.h $(PROTO)/ns_proto_fit.h

.PHONY: test fixtures clean

test: $(BIN)
	$(BIN) $(FIXTURES)

fixtures: $(BIN)
	mkdir -p $(FIXTURES)/framing
	$(BIN) $(FIXTURES) --write

$(BIN): $(SRCS) $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
