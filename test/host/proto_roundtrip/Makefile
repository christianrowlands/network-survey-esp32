# Host round-trip test for the generated nanopb code. No ESP-IDF needed.
#   make            build and check against test/fixtures/proto
#   make fixtures   rewrite the fixtures (after a deliberate proto change)

ROOT     := ../../..
PROTO    := $(ROOT)/firmware/components/ns_proto
FIXTURES := $(ROOT)/test/fixtures/proto
BUILD    := ../build/proto_roundtrip
BIN      := $(BUILD)/proto_roundtrip

CC     ?= cc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -I$(PROTO) -I$(PROTO)/nanopb
SRCS   := main.c $(PROTO)/ns_board.pb.c $(PROTO)/ns_proto_fit.c $(PROTO)/nanopb/pb_common.c \
          $(PROTO)/nanopb/pb_encode.c $(PROTO)/nanopb/pb_decode.c

.PHONY: test fixtures clean

test: $(BIN)
	$(BIN) $(FIXTURES)

fixtures: $(BIN)
	mkdir -p $(FIXTURES)
	$(BIN) $(FIXTURES) --write

$(BIN): $(SRCS) $(PROTO)/ns_board.pb.h $(PROTO)/ns_proto_fit.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
