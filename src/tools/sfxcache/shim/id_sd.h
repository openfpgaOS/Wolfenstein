/* Shim for the native sfxcache tool: dbopl needs only the volume scale
 * from the real id_sd.h.  Must match src/wolfenstein/port/id_sd.h. */
#pragma once
static const int MAX_VOLUME = 20;
#define MULTIPLY_VOLUME(v) (((v) * 10.0f + 3.0f) / (MAX_VOLUME * 10.0f + 3.0f))
