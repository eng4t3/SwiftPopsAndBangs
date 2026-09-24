#pragma once
// =========================================================================================
// SETTINGS PROFILES (v2.2) - accessors for other modules (implemented in swift_show_tuning.ino)
// =========================================================================================
// Three slots (0 UTCA, 1 SHOW, 2 RAJT by default); the active slot is the running config.
// Contract: docs/PROTOCOL.md, "Profiles (v2.2)". Both functions are safe from any task.
// =========================================================================================
#include <stddef.h>
#include <stdint.h>

#define PROFILE_COUNT       3
#define PROFILE_NAME_CHARS  12   // characters (UTF-8 code points)
#define PROFILE_NAME_BYTES  24   // bytes, without the terminating NUL

// Active slot, 0..PROFILE_COUNT-1.
uint8_t profileActiveIndex();

// Copies the active slot's name (UTF-8, NUL-terminated, <= PROFILE_NAME_BYTES bytes) into out.
void profileActiveName(char* out, size_t cap);
