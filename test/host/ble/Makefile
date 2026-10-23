# Host tests for the BLE sniffer's AD parser and advertiser table.

ROOT   := ../../..
COMP   := $(ROOT)/firmware/components
PROTO  := $(COMP)/ns_proto
BLE    := $(COMP)/ble_sniffer
BUILD  := ../build/ble
BIN    := $(BUILD)/ble

CC       ?= cc
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O1 -g $(SANITIZE) -I$(BLE)/include -I$(PROTO) -I$(PROTO)/nanopb
SRCS     := main.c $(BLE)/ble_ad.c $(BLE)/ble_agg.c $(PROTO)/ns_board.pb.c $(PROTO)/ns_proto_fit.c \
            $(PROTO)/nanopb/pb_common.c $(PROTO)/nanopb/pb_encode.c $(PROTO)/nanopb/pb_decode.c
HDRS     := $(BLE)/include/ble_ad.h $(BLE)/include/ble_agg.h

.PHONY: test clean

test: $(BIN)
	$(BIN)

$(BIN): $(SRCS) $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
