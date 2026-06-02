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
void OF_WolfPerf_FrameStart(void);
void OF_WolfPerf_Add(OFWolfPerfPhase phase, uint32_t start_us);
void OF_WolfPerf_AddTicks(unsigned int count);
void OF_WolfPerf_FrameEnd(void);
#else
static inline uint32_t OF_WolfPerf_NowUS(void) { return 0; }
static inline void OF_WolfPerf_FrameStart(void) {}
static inline void OF_WolfPerf_Add(OFWolfPerfPhase, uint32_t) {}
static inline void OF_WolfPerf_AddTicks(unsigned int) {}
static inline void OF_WolfPerf_FrameEnd(void) {}
#endif

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
void OF_WolfGPU_Init(void);
void OF_WolfGPU_Shutdown(void);
bool OF_WolfGPU_CanUseVideoFrames(int width, int height);
bool OF_WolfGPU_AcquireVideoFrame(uint8_t **framebuffer, int *pitch,
	int width, int height);
void OF_WolfGPU_SetNextVideoFramePreserve(bool preserve);
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
bool OF_WolfGPU_DrawSpan(uint8_t *dest, int count, const uint8_t *source,
	int tex_width, int tex_height, int sfrac, int tfrac,
	int sstep, int tstep, int light);
bool OF_WolfGPU_ClearSpan(uint8_t *dest, int count, uint8_t color);
bool OF_WolfGPU_ClearRect(uint8_t *dest, int width, int height, uint8_t color);
#else
static inline void OF_WolfGPU_Init(void) {}
static inline void OF_WolfGPU_Shutdown(void) {}
static inline bool OF_WolfGPU_CanUseVideoFrames(int, int) { return false; }
static inline bool OF_WolfGPU_AcquireVideoFrame(uint8_t **, int *,
	int, int) { return false; }
static inline void OF_WolfGPU_SetNextVideoFramePreserve(bool) {}
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
static inline bool OF_WolfGPU_DrawSpan(uint8_t *, int, const uint8_t *,
	int, int, int, int, int, int, int) { return false; }
static inline bool OF_WolfGPU_ClearSpan(uint8_t *, int, uint8_t) { return false; }
static inline bool OF_WolfGPU_ClearRect(uint8_t *, int, int, uint8_t) { return false; }
#endif

#endif
