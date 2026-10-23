# Host tests for the OTA transfer state machine. No ESP-IDF needed.

ROOT  := ../../..
OTA   := $(ROOT)/firmware/components/ota
BUILD := ../build/ota
BIN   := $(BUILD)/ota_test

CC       ?= cc
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O1 -g $(SANITIZE) -I$(OTA)
SRCS     := main.c $(OTA)/ota_sm.c

.PHONY: test clean

test: $(BIN)
	$(BIN)

$(BIN): $(SRCS) $(OTA)/ota_sm.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
