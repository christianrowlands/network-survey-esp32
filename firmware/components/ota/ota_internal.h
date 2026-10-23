// Shared between ota.c (transfer) and ota_confirm.c (rollback).
#pragma once

#include <stdbool.h>

// Reads the running image's state; call once at init.
void ota_confirm_init(void);

// Marks a pending image valid (cancels the rollback). Safe from any task;
// returns true if it was pending.
bool ota_confirm(const char *why);

// Link hook: confirms on a compatible Hello when the self-test passes, and
// returns true for the first Hello after the image was confirmed.
bool ota_confirm_on_hello(void);
