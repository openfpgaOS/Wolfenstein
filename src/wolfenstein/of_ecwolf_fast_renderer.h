#ifndef OF_ECWOLF_FAST_RENDERER_H
#define OF_ECWOLF_FAST_RENDERER_H

#include <stdbool.h>

#if defined(OF_ECWOLF_FAST_RENDERER) && defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
#include "wl_def.h"
bool OF_WolfFastRenderer_RenderWalls(byte *framebuffer, unsigned pitch);
void OF_WolfFastRenderer_Reset(void);
#else
static inline bool OF_WolfFastRenderer_RenderWalls(byte *, unsigned)
{
	return false;
}
static inline void OF_WolfFastRenderer_Reset(void) {}
#endif

#endif
