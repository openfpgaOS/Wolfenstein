#ifndef OF_ECWOLF_GPU_H
#define OF_ECWOLF_GPU_H

#include <stdbool.h>
#include <stdint.h>

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC) && !defined(OF_ECWOLF_PERF_DISABLED)
#define OF_ECWOLF_PERF_ENABLED 1
#else
#define OF_ECWOLF_PERF_ENABLED 0
#endif

typedef enum OFWolfPerfPhase
{
	OF_WOLF_PERF_EVENTS,
	OF_WOLF_PERF_SIM,
	OF_WOLF_PERF_SIM_SNAPSHOT,
	OF_WOLF_PERF_SIM_CONTROLS,
	OF_WOLF_PERF_SIM_SPAWN,
	OF_WOLF_PERF_SIM_THINKERS,
	OF_WOLF_PERF_SIM_FINISH,
	OF_WOLF_PERF_SIM_GC,
	OF_WOLF_PERF_RENDER,
	OF_WOLF_PERF_VIEW_SETUP,
	OF_WOLF_PERF_WALLS,
	OF_WOLF_PERF_FLOORCEIL,
	OF_WOLF_PERF_SKY,
	OF_WOLF_PERF_SPRITES,
	OF_WOLF_PERF_WEAPON,
	OF_WOLF_PERF_OVERLAY,
	OF_WOLF_PERF_PRESENT,
	OF_WOLF_PERF_ACQUIRE,
	OF_WOLF_PERF_SOUND,
	OF_WOLF_PERF_MAINT,
	OF_WOLF_PERF_GPU_FINISH,
	OF_WOLF_PERF_RENDER_LOCK,
	OF_WOLF_PERF_RENDER_BEGIN,
	OF_WOLF_PERF_RENDER_CLEAR,
	OF_WOLF_PERF_RENDER_UNLOCK,
	OF_WOLF_PERF_RENDER_MISC,
	OF_WOLF_PERF_COUNT
} OFWolfPerfPhase;

#if OF_ECWOLF_PERF_ENABLED
uint32_t OF_WolfPerf_NowUS(void);
void OF_WolfPerf_SetThinkerSampling(int enabled);
int OF_WolfPerf_ThinkerSamplingEnabled(void);
void OF_WolfPerf_FrameStart(void);
void OF_WolfPerf_Add(OFWolfPerfPhase phase, uint32_t start_us);
void OF_WolfPerf_AddTicks(unsigned int count);
void OF_WolfPerf_FrameEnd(void);
#else
static inline uint32_t OF_WolfPerf_NowUS(void) { return 0; }
static inline void OF_WolfPerf_SetThinkerSampling(int enabled) { (void)enabled; }
static inline int OF_WolfPerf_ThinkerSamplingEnabled(void) { return 0; }
static inline void OF_WolfPerf_FrameStart(void) {}
static inline void OF_WolfPerf_Add(OFWolfPerfPhase, uint32_t) {}
static inline void OF_WolfPerf_AddTicks(unsigned int) {}
static inline void OF_WolfPerf_FrameEnd(void) {}
#endif

/* A single perspective-correct texture plane for a floor/ceiling half.  Each
 * attribute is evaluated by the GPU as base + u*du + v*dv, where u is the minor
 * (per-pixel, along the span) axis and v is the major (per-scanline) axis.
 * szi/tzi are the texel*zi numerators ((texel_q16 * zi) >> 16) and zi is the
 * perspective denominator (1/z); the GPU divides szi/zi and tzi/zi per pixel.
 * light is a Q6.16 shade-row ramp (low 6 bits select the palookup row). One
 * plane drives every scanline of the half through a single param-span command
 * instead of one affine span per row. */
typedef struct OFWolfTexturedPlane
{
	int32_t szi_origin, szi_du, szi_dv;
	int32_t tzi_origin, tzi_du, tzi_dv;
	int32_t zi_origin,  zi_du,  zi_dv;
	int32_t light_origin, light_du, light_dv;
} OFWolfTexturedPlane;

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
void OF_WolfGPU_Init(void);
void OF_WolfGPU_Shutdown(void);
void OF_WolfGPU_ApplyRefreshPolicy(void);
bool OF_WolfGPU_CanUseVideoFrames(int width, int height);
bool OF_WolfGPU_AcquireVideoFrame(uint8_t **framebuffer, int *pitch,
	int width, int height);
void OF_WolfGPU_SetNextVideoFramePreserve(bool preserve);
void OF_WolfGPU_SetNextVideoFramePreserveExcludeRows(int y0, int y1);
bool OF_WolfGPU_USSinceDisplaySync(uint32_t *us_out);
bool OF_WolfGPU_FlipVideoBuffer(int idx, uint32_t *token);
bool OF_WolfGPU_PresentVideoFrame(void);
void OF_WolfGPU_ResetVideoFrames(void);
void OF_WolfGPU_BeginFrame(uint8_t *framebuffer, int pitch, int height);
void OF_WolfGPU_EndFrame(void);
void OF_WolfGPU_FallbackToCPU(void);
void OF_WolfGPU_PrepareForCPUAccessRect(uint8_t *dest, int width, int height,
	int pitch);
void OF_WolfGPU_PrepareForCPUAccessColumn(uint8_t *dest, int count, int pitch);
bool OF_WolfGPU_IsActive(void);
bool OF_WolfGPU_DrawColumn(uint8_t *dest, int count, const uint8_t *source,
	int source_len, int texfrac, int texstep, int light);
bool OF_WolfGPU_DrawMaskedColumn(uint8_t *dest, int count,
	const uint8_t *source, int source_len, int texfrac, int texstep,
	int light);
bool OF_WolfGPU_DrawRawColumn(uint8_t *dest, int count,
	const uint8_t *source, int source_len, int texfrac, int texstep);
void OF_WolfGPU_PreloadSource(const uint8_t *source, uint32_t bytes);
void OF_WolfGPU_SourceBuffersChanged(void);
bool OF_WolfGPU_DrawSpan(uint8_t *dest, int count, const uint8_t *source,
	int tex_width, int tex_height, int sfrac, int tfrac,
	int sstep, int tstep, int light);
bool OF_WolfGPU_ClearSpan(uint8_t *dest, int count, uint8_t color);
bool OF_WolfGPU_ClearRect(uint8_t *dest, int width, int height, uint8_t color);
bool OF_WolfGPU_HasTexturedPlane(void);
bool OF_WolfGPU_DrawTexturedPlane(uint8_t *dest, int pitch, int rows, int count,
	const uint8_t *source, int tex_width, int tex_height,
	const OFWolfTexturedPlane *plane);
#else
static inline void OF_WolfGPU_Init(void) {}
static inline void OF_WolfGPU_Shutdown(void) {}
static inline void OF_WolfGPU_ApplyRefreshPolicy(void) {}
static inline bool OF_WolfGPU_CanUseVideoFrames(int, int) { return false; }
static inline bool OF_WolfGPU_AcquireVideoFrame(uint8_t **, int *,
	int, int) { return false; }
static inline void OF_WolfGPU_SetNextVideoFramePreserve(bool) {}
static inline void OF_WolfGPU_SetNextVideoFramePreserveExcludeRows(int, int) {}
static inline bool OF_WolfGPU_USSinceDisplaySync(uint32_t *) { return false; }
static inline bool OF_WolfGPU_FlipVideoBuffer(int, uint32_t *) { return false; }
static inline bool OF_WolfGPU_PresentVideoFrame(void) { return false; }
static inline void OF_WolfGPU_ResetVideoFrames(void) {}
static inline void OF_WolfGPU_BeginFrame(uint8_t *, int, int) {}
static inline void OF_WolfGPU_EndFrame(void) {}
static inline void OF_WolfGPU_FallbackToCPU(void) {}
static inline void OF_WolfGPU_PrepareForCPUAccessRect(uint8_t *, int, int, int) {}
static inline void OF_WolfGPU_PrepareForCPUAccessColumn(uint8_t *, int, int) {}
static inline bool OF_WolfGPU_IsActive(void) { return false; }
static inline bool OF_WolfGPU_DrawColumn(uint8_t *, int, const uint8_t *,
	int, int, int, int) { return false; }
static inline bool OF_WolfGPU_DrawMaskedColumn(uint8_t *, int,
	const uint8_t *, int, int, int, int) { return false; }
static inline bool OF_WolfGPU_DrawRawColumn(uint8_t *, int,
	const uint8_t *, int, int, int) { return false; }
static inline void OF_WolfGPU_PreloadSource(const uint8_t *, uint32_t) {}
static inline void OF_WolfGPU_SourceBuffersChanged(void) {}
static inline bool OF_WolfGPU_DrawSpan(uint8_t *, int, const uint8_t *,
	int, int, int, int, int, int, int) { return false; }
static inline bool OF_WolfGPU_ClearSpan(uint8_t *, int, uint8_t) { return false; }
static inline bool OF_WolfGPU_ClearRect(uint8_t *, int, int, uint8_t) { return false; }
static inline bool OF_WolfGPU_HasTexturedPlane(void) { return false; }
static inline bool OF_WolfGPU_DrawTexturedPlane(uint8_t *, int, int, int,
	const uint8_t *, int, int, const OFWolfTexturedPlane *) { return false; }
#endif

#endif
