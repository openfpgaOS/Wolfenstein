/* Shim for the native sfxcache tool: mame/fmopl includes wl_def.h for the
 * engine type aliases only. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t byte;
typedef uint16_t word;
typedef uint32_t longword;
typedef int32_t fixed;
#define PI 3.141592657
