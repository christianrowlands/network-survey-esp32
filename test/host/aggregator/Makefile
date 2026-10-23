# Host tests for the aggregator (with the parser and the nanopb runtime).

ROOT   := ../../..
COMP   := $(ROOT)/firmware/components
PROTO  := $(COMP)/ns_proto
BUILD  := ../build/aggregator
BIN    := $(BUILD)/aggregator

CC       ?= cc
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O1 -g $(SANITIZE) -I$(COMP)/aggregator/include \
            -I$(COMP)/dot11_parser/include -I$(PROTO) -I$(PROTO)/nanopb
SRCS     := main.c $(COMP)/aggregator/aggregator.c $(COMP)/dot11_parser/dot11_parser.c \
            $(COMP)/dot11_parser/dot11_ies.c $(PROTO)/ns_board.pb.c $(PROTO)/ns_proto_fit.c \
            $(PROTO)/nanopb/pb_common.c $(PROTO)/nanopb/pb_encode.c $(PROTO)/nanopb/pb_decode.c
HDRS     := $(COMP)/aggregator/include/aggregator.h $(COMP)/dot11_parser/include/dot11_parser.h \
            ../common/frame_builder.h

.PHONY: test clean

test: $(BIN)
	$(BIN)

$(BIN): $(SRCS) $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
