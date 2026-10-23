# Host tests for the pairing and bond policy (docs/protocol.md section 3).

ROOT    := ../../..
GATT    := $(ROOT)/firmware/components/gatt_server
BUILD   := ../build/pairing
BIN     := $(BUILD)/pairing

CC       ?= cc
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O1 -g $(SANITIZE) -I$(GATT)
SRCS     := main.c $(GATT)/pairing_policy.c $(GATT)/conn_track.c

.PHONY: test clean

test: $(BIN)
	$(BIN)

$(BIN): $(SRCS) $(GATT)/pairing_policy.h $(GATT)/conn_track.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
