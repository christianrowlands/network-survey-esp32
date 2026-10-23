# Host tests for the GPS NMEA parser.

ROOT   := ../../..
COMP   := $(ROOT)/firmware/components
PROTO  := $(COMP)/ns_proto
GPS    := $(COMP)/gps
BUILD  := ../build/nmea
BIN    := $(BUILD)/nmea

CC       ?= cc
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O1 -g $(SANITIZE) -I$(GPS)/include -I$(PROTO) -I$(PROTO)/nanopb
SRCS     := main.c $(GPS)/nmea.c $(PROTO)/ns_board.pb.c $(PROTO)/ns_proto_fit.c \
            $(PROTO)/nanopb/pb_common.c $(PROTO)/nanopb/pb_encode.c $(PROTO)/nanopb/pb_decode.c
HDRS     := $(GPS)/include/nmea.h

.PHONY: test clean

test: $(BIN)
	$(BIN)

$(BIN): $(SRCS) $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

clean:
	rm -rf $(BUILD)
