#include "of_ecwolf_gpu.h"

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "of_analogizer.h"
#include "of_cache.h"
#include "of_caps.h"
#include "of_gpu.h"
#include "of_timer.h"
#include "of_video.h"
#include "r_data/colormaps.h"
#include "wl_def.h"

static bool gpu_available;
static bool gpu_initialized;
static bool gpu_colormap_uploaded;
static bool gpu_frame_active;
static bool gpu_frame_dirty;
static uint8_t *gpu_framebuffer;
static int gpu_pitch;
static int gpu_height;

static bool gpu_video_initialized;
static bool gpu_video_acquire_pending;
static int gpu_video_draw_idx = -1;
static int gpu_video_acquire_idx = -1;
static uint32_t gpu_video_acquire_token;
static uint8_t *gpu_video_draw_fb;
static uint8_t *gpu_video_last_fb;
static int gpu_video_pitch;
static int gpu_video_height;
static uint32_t gpu_video_frame_bytes;
static bool gpu_video_next_frame_preserve = true;
/* Rows [skip_y0, skip_y1) of the next preserved frame are fully redrawn by
 * the renderer (full-width 3D view), so the acquire-time preserve copy can
 * skip them.  Consumed (and reset) by the next buffer acquire. */
static int gpu_video_preserve_skip_y0;
static int gpu_video_preserve_skip_y1;
static bool gpu_video_clean_first_begin;
static bool gpu_video_compat_logged;
/* Last (width, height, pitch) validated against the hardware video mode.
 * Lets the per-frame acquire skip the analogizer/refresh/get-mode service
 * calls (of_video_set_refresh_vtotal also resets the kernel's adaptive
 * refresh state, so it must not run every frame).  Cleared on reset. */
static int gpu_video_mode_ok_w;
static int gpu_video_mode_ok_h;
static int gpu_video_mode_ok_pitch;

/* Diagnostics for the transient-artifact hunt (reported in the perf line):
 *  - rejects: draw helpers refused a primitive while a GPU frame was active
 *    (those columns are CPU-drawn after a region sync; a high rate means the
 *    GPU eligibility checks are being missed).
 *  - fence_late: the previous frame's CMD_FLIP fence had not retired when
 *    the next buffer acquire started (GPU still drawing into sim time).
 *  - forced_swaps: the fence still had not retired after acquire returned —
 *    the kernel's bounded wait expired and it CPU-forced the swap, so a
 *    partially rendered frame may have been scanned out (visible tear). */
static uint32_t gpu_dbg_rejects;
static uint32_t gpu_dbg_fence_late;
static uint32_t gpu_dbg_forced_swaps;

/* Timestamp (of_time_us) of the last acquire that actually blocked on the
 * display flip fence.  The flip fence retires when the display consumes the
 * previous swap, so this is in effect the last vsync as seen by the app --
 * the render interpolation re-anchors its time sample on it to keep motion
 * sampling locked to the display clock instead of "now". */
static uint32_t gpu_video_pace_us;
static bool gpu_video_pace_valid;

static of_gpu_affine_span_group_t gpu_batch;
static int gpu_batch_count;
static uint8_t gpu_batch_flags;
static int gpu_batch_tex_width;
static int gpu_batch_tex_w_mask;
static int gpu_batch_tex_h_mask;
static int gpu_batch_fb_step;

/* Region-scoped CPU<->GPU coherence.
 *
 * The GPU is an asynchronous span rasterizer: draw commands are staged and the
 * GPU only runs them when kicked (at present).  The expensive operations are a
 * blocking of_gpu_finish() fence wait and whole-framebuffer cache flush/
 * invalidate.  When a primitive cannot go on the GPU (an odd-sized sprite
 * column, a non-power-of-two parallax sky column, ...) the renderer draws that
 * one column on the CPU into the same buffer the GPU is filling.  Instead of
 * draining the GPU and walking the entire frame for every such fallback (which
 * also forced the rest of the frame onto the CPU), we drain only when GPU work
 * is genuinely outstanding and sync only the cache lines the CPU touches,
 * leaving the GPU frame active so later eligible primitives keep batching.
 *
 * This mirrors the Doom OpenFPGA renderer's proven flip-path model
 * (../Doom/src/doom/cdoom/doom/r_gpu.c: gpu_cpu_dirty_lines / gpu_cpu_valid_lines
 * / gpu_invalidate_rect_for_cpu / gpu_flush_cpu_dirty_lines). */

#define GPU_FB_TRACK_LINE_BYTES 64u
#define GPU_FB_TRACK_MAX_BYTES (512u * 1024u)
#define GPU_FB_TRACK_MAX_LINES (GPU_FB_TRACK_MAX_BYTES / GPU_FB_TRACK_LINE_BYTES)
#define GPU_FB_TRACK_WORDS ((GPU_FB_TRACK_MAX_LINES + 31u) / 32u)

static uint8_t *gpu_track_base;
static uint32_t gpu_track_bytes;
static int gpu_track_pitch;
static bool gpu_cpu_dirty;
static uint32_t gpu_cpu_dirty_lines[GPU_FB_TRACK_WORDS];
static uint32_t gpu_cpu_valid_lines[GPU_FB_TRACK_WORDS];

static const int gpu_batch_max_lanes =
	(int)OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES;
static const int gpu_source_cache_size = 512;
static const int gpu_mask_cache_size = 1024;

struct GpuSourceCacheRange
{
	uintptr_t start;
	uintptr_t end;
};

struct GpuMaskCacheEntry
{
	const uint8_t *source;
	int source_len;
	uint8_t state;
};

static GpuSourceCacheRange gpu_source_cache[gpu_source_cache_size];
static unsigned int gpu_source_cache_next;
static GpuSourceCacheRange gpu_source_cache_last;
static GpuMaskCacheEntry gpu_mask_cache[gpu_mask_cache_size];
static GpuMaskCacheEntry gpu_mask_cache_last;
/* Set when texture pixel buffers were freed/recomposited; the GPU's internal
 * texture cache is flushed at the next frame start (when the GPU is idle). */
static bool gpu_tex_flush_pending;

#if OF_ECWOLF_PERF_ENABLED
static uint64_t wolf_perf_accum[OF_WOLF_PERF_COUNT];
static uint64_t wolf_perf_frame_total;
static uint64_t wolf_perf_tic_total;
static uint32_t wolf_perf_window_start_us;
static uint32_t wolf_perf_frame_start_us;
static unsigned int wolf_perf_frames;
static bool wolf_perf_frame_active;
static const uint32_t wolf_perf_report_us = 10000000u;

static unsigned int wolf_perf_avg(OFWolfPerfPhase phase)
{
	if(wolf_perf_frames == 0)
		return 0;
	return (unsigned int)(wolf_perf_accum[phase] / wolf_perf_frames);
}

uint32_t OF_WolfPerf_NowUS(void)
{
	return of_time_us();
}

void OF_WolfPerf_FrameStart(void)
{
	const uint32_t now = of_time_us();
	if(wolf_perf_window_start_us == 0)
		wolf_perf_window_start_us = now;
	wolf_perf_frame_start_us = now;
	wolf_perf_frame_active = true;
}

void OF_WolfPerf_Add(OFWolfPerfPhase phase, uint32_t start_us)
{
	if(!wolf_perf_frame_active || phase < 0 || phase >= OF_WOLF_PERF_COUNT)
		return;
	wolf_perf_accum[phase] += (uint32_t)(of_time_us() - start_us);
}

void OF_WolfPerf_AddTicks(unsigned int count)
{
	if(wolf_perf_frame_active)
		wolf_perf_tic_total += count;
}

void OF_WolfPerf_FrameEnd(void)
{
	if(!wolf_perf_frame_active)
		return;

	const uint32_t now = of_time_us();
	wolf_perf_frame_total += (uint32_t)(now - wolf_perf_frame_start_us);
	wolf_perf_frames++;
	wolf_perf_frame_active = false;

	const uint32_t elapsed = now - wolf_perf_window_start_us;
	if(elapsed < wolf_perf_report_us)
		return;

	const unsigned int fps_x10 =
		(unsigned int)((uint64_t)wolf_perf_frames * 10000000ull / elapsed);
	const unsigned int tics_x10 =
		(unsigned int)(wolf_perf_tic_total * 10ull / wolf_perf_frames);
	const unsigned int frame_avg =
		(unsigned int)(wolf_perf_frame_total / wolf_perf_frames);

	printf("perf %u.%u fr=%u ev=%u sim=%u sn=%u ctl=%u spn=%u th=%u fin=%u gc=%u r=%u lk=%u bg=%u cl=%u rm=%u st=%u wl=%u fl=%u sk=%u spr=%u wp=%u ul=%u ov=%u pr=%u aq=%u sd=%u mt=%u gw=%u rj=%u lt=%u fw=%u t=%u.%u\n",
		fps_x10 / 10, fps_x10 % 10, frame_avg,
		wolf_perf_avg(OF_WOLF_PERF_EVENTS),
		wolf_perf_avg(OF_WOLF_PERF_SIM),
		wolf_perf_avg(OF_WOLF_PERF_SIM_SNAPSHOT),
		wolf_perf_avg(OF_WOLF_PERF_SIM_CONTROLS),
		wolf_perf_avg(OF_WOLF_PERF_SIM_SPAWN),
		wolf_perf_avg(OF_WOLF_PERF_SIM_THINKERS),
		wolf_perf_avg(OF_WOLF_PERF_SIM_FINISH),
		wolf_perf_avg(OF_WOLF_PERF_SIM_GC),
		wolf_perf_avg(OF_WOLF_PERF_RENDER),
		wolf_perf_avg(OF_WOLF_PERF_RENDER_LOCK),
		wolf_perf_avg(OF_WOLF_PERF_RENDER_BEGIN),
		wolf_perf_avg(OF_WOLF_PERF_RENDER_CLEAR),
		wolf_perf_avg(OF_WOLF_PERF_RENDER_MISC),
		wolf_perf_avg(OF_WOLF_PERF_VIEW_SETUP),
		wolf_perf_avg(OF_WOLF_PERF_WALLS),
		wolf_perf_avg(OF_WOLF_PERF_FLOORCEIL),
		wolf_perf_avg(OF_WOLF_PERF_SKY),
		wolf_perf_avg(OF_WOLF_PERF_SPRITES),
		wolf_perf_avg(OF_WOLF_PERF_WEAPON),
		wolf_perf_avg(OF_WOLF_PERF_RENDER_UNLOCK),
		wolf_perf_avg(OF_WOLF_PERF_OVERLAY),
		wolf_perf_avg(OF_WOLF_PERF_PRESENT),
		wolf_perf_avg(OF_WOLF_PERF_ACQUIRE),
		wolf_perf_avg(OF_WOLF_PERF_SOUND),
		wolf_perf_avg(OF_WOLF_PERF_MAINT),
		wolf_perf_avg(OF_WOLF_PERF_GPU_FINISH),
		(unsigned int)gpu_dbg_rejects,
		(unsigned int)gpu_dbg_fence_late,
		(unsigned int)gpu_dbg_forced_swaps,
		tics_x10 / 10, tics_x10 % 10);

	memset(wolf_perf_accum, 0, sizeof(wolf_perf_accum));
	wolf_perf_frame_total = 0;
	wolf_perf_tic_total = 0;
	wolf_perf_window_start_us = now;
	wolf_perf_frames = 0;
	gpu_dbg_rejects = 0;
	gpu_dbg_fence_late = 0;
	gpu_dbg_forced_swaps = 0;
}
#endif

static bool gpu_is_power_of_two(int value)
{
	return value > 0 && (value & (value - 1)) == 0;
}

/* A draw helper refused a primitive.  Only counted while a GPU frame is
 * active: rejects during legitimate CPU phases (menus, automap) are normal. */
static bool gpu_reject(void)
{
	if(gpu_frame_active)
		gpu_dbg_rejects++;
	return false;
}

static void gpu_flush_batch()
{
	if(gpu_batch_count <= 0)
		return;

	gpu_batch.lane_count = (uint8_t)gpu_batch_count;
	of_gpu_draw_affine_span_group(&gpu_batch);
	gpu_batch_count = 0;
	gpu_frame_dirty = true;
}

static void gpu_reset_batch()
{
	memset(&gpu_batch, 0, sizeof(gpu_batch));
	gpu_batch_count = 0;
	gpu_batch_flags = 0;
	gpu_batch_tex_width = 0;
	gpu_batch_tex_w_mask = 0;
	gpu_batch_tex_h_mask = 0;
	gpu_batch_fb_step = 0;
}

static void gpu_reset_source_cache()
{
	memset(gpu_source_cache, 0, sizeof(gpu_source_cache));
	gpu_source_cache_next = 0;
	gpu_source_cache_last.start = 0;
	gpu_source_cache_last.end = 0;
}

static uint8_t gpu_classify_mask_source(const uint8_t *source, int source_len)
{
	if(source == NULL || source_len <= 0)
		return 3;

	if(gpu_mask_cache_last.source == source &&
		gpu_mask_cache_last.source_len == source_len)
	{
		return gpu_mask_cache_last.state;
	}

	const uintptr_t key =
		((uintptr_t)source >> 2) ^ ((uintptr_t)source >> 11) ^
		(uintptr_t)source_len;
	GpuMaskCacheEntry &slot =
		gpu_mask_cache[key & (uintptr_t)(gpu_mask_cache_size - 1)];
	if(slot.source == source && slot.source_len == source_len)
	{
		gpu_mask_cache_last = slot;
		return slot.state;
	}

	bool sawZero = false;
	bool sawOpaque = false;
	for(int i = 0;i < source_len;i++)
	{
		if(source[i] == 0)
			sawZero = true;
		else
			sawOpaque = true;
		if(sawZero && sawOpaque)
			break;
	}

	uint8_t state = 3;
	if(!sawOpaque)
		state = 1;
	else if(!sawZero)
		state = 2;

	slot.source = source;
	slot.source_len = source_len;
	slot.state = state;
	gpu_mask_cache_last = slot;
	return state;
}

static void gpu_make_source_visible(const uint8_t *source, uint32_t bytes)
{
	if(source == NULL || bytes == 0)
		return;

	uintptr_t start = (uintptr_t)source;
	uintptr_t end = start + bytes;
	if(end < start)
		return;

	uintptr_t aligned_start = start & ~(uintptr_t)(OF_GPU_CACHE_LINE_BYTES - 1);
	uintptr_t aligned_end = (end + OF_GPU_CACHE_LINE_BYTES - 1) &
		~(uintptr_t)(OF_GPU_CACHE_LINE_BYTES - 1);

	if(aligned_start >= gpu_source_cache_last.start &&
		aligned_end <= gpu_source_cache_last.end)
	{
		return;
	}

	for(int i = 0;i < gpu_source_cache_size;i++)
	{
		if(aligned_start >= gpu_source_cache[i].start &&
			aligned_end <= gpu_source_cache[i].end)
		{
			gpu_source_cache_last = gpu_source_cache[i];
			return;
		}
	}

	of_cache_flush_range((void *)source, bytes);

	GpuSourceCacheRange &slot =
		gpu_source_cache[gpu_source_cache_next++ % gpu_source_cache_size];
	slot.start = aligned_start;
	slot.end = aligned_end;
	gpu_source_cache_last = slot;
}

static void gpu_line_set(uint32_t *bits, unsigned int line)
{
	bits[line >> 5] |= 1u << (line & 31u);
}

static bool gpu_line_test(const uint32_t *bits, unsigned int line)
{
	return (bits[line >> 5] & (1u << (line & 31u))) != 0;
}

static void gpu_clear_line_bits(uint32_t *bits)
{
	memset(bits, 0, sizeof(gpu_cpu_dirty_lines));
}

static void gpu_reset_cpu_cache_tracking(void)
{
	gpu_clear_line_bits(gpu_cpu_dirty_lines);
	gpu_clear_line_bits(gpu_cpu_valid_lines);
	gpu_cpu_dirty = false;
}

static unsigned int gpu_track_line_count(void)
{
	uint32_t lines = (gpu_track_bytes + GPU_FB_TRACK_LINE_BYTES - 1u) /
		GPU_FB_TRACK_LINE_BYTES;
	return lines > GPU_FB_TRACK_MAX_LINES ? GPU_FB_TRACK_MAX_LINES :
		(unsigned int)lines;
}

static void gpu_flush_line_run(unsigned int first, unsigned int last)
{
	if(gpu_track_base == NULL || first > last)
		return;

	uint32_t start = first * GPU_FB_TRACK_LINE_BYTES;
	uint32_t size = (last - first + 1u) * GPU_FB_TRACK_LINE_BYTES;
	if(start >= gpu_track_bytes)
		return;
	if(start + size > gpu_track_bytes)
		size = gpu_track_bytes - start;
	of_cache_flush_range(gpu_track_base + start, size);
}

static void gpu_inval_line_run(unsigned int first, unsigned int last)
{
	if(gpu_track_base == NULL || first > last)
		return;

	uint32_t start = first * GPU_FB_TRACK_LINE_BYTES;
	uint32_t size = (last - first + 1u) * GPU_FB_TRACK_LINE_BYTES;
	if(start >= gpu_track_bytes)
		return;
	if(start + size > gpu_track_bytes)
		size = gpu_track_bytes - start;
	of_cache_inval_range(gpu_track_base + start, size);
}

/* Write back (publish to SDRAM) only the cache lines the CPU has dirtied this
 * frame, coalescing contiguous runs.  Called before the page flip so CPU
 * overlay pixels land in the buffer the display controller scans out. */
static void gpu_flush_cpu_dirty_lines(void)
{
	if(!gpu_cpu_dirty || gpu_track_base == NULL)
		return;

	unsigned int count = gpu_track_line_count();
	unsigned int line = 0;
	while(line < count)
	{
		while(line < count && !gpu_line_test(gpu_cpu_dirty_lines, line))
			line++;
		if(line >= count)
			break;

		unsigned int first = line;
		while(line < count && gpu_line_test(gpu_cpu_dirty_lines, line))
			line++;

		gpu_flush_line_run(first, line - 1u);
	}

	gpu_clear_line_bits(gpu_cpu_dirty_lines);
	gpu_cpu_dirty = false;
}

/* Drop stale CPU cache lines covering a region so the CPU reads GPU-produced
 * pixels.  Skips lines already CPU-dirty (so pending CPU writes are not lost)
 * or already invalidated this frame, and marks the freshly invalidated lines
 * valid.  Caller must have ensured the GPU has finished writing the region. */
static void gpu_invalidate_rect_for_cpu(const uint8_t *dest, int w, int h,
	int pitch)
{
	if(gpu_track_base == NULL || dest == NULL || w <= 0 || h <= 0 ||
		(const uint8_t *)dest < gpu_track_base)
	{
		return;
	}

	uintptr_t base_off = (uintptr_t)dest - (uintptr_t)gpu_track_base;
	unsigned int count = gpu_track_line_count();

	for(int row = 0; row < h; row++)
	{
		uintptr_t off = base_off + (uintptr_t)row * (uintptr_t)pitch;
		if(off >= gpu_track_bytes)
			break;
		uintptr_t endoff = off + (uintptr_t)w - 1u;
		if(endoff >= gpu_track_bytes)
			endoff = gpu_track_bytes - 1u;

		unsigned int first = (unsigned int)(off / GPU_FB_TRACK_LINE_BYTES);
		unsigned int last = (unsigned int)(endoff / GPU_FB_TRACK_LINE_BYTES);
		if(last >= count)
			last = count - 1u;

		unsigned int line = first;
		while(line <= last)
		{
			while(line <= last &&
				(gpu_line_test(gpu_cpu_dirty_lines, line) ||
				 gpu_line_test(gpu_cpu_valid_lines, line)))
			{
				line++;
			}
			if(line > last)
				break;

			unsigned int run_first = line;
			while(line <= last &&
				!gpu_line_test(gpu_cpu_dirty_lines, line) &&
				!gpu_line_test(gpu_cpu_valid_lines, line))
			{
				gpu_line_set(gpu_cpu_valid_lines, line);
				line++;
			}

			gpu_inval_line_run(run_first, line - 1u);
		}
	}
}

/* Mark the cache lines covering a region as CPU-dirty (and valid) so they are
 * written back at present. */
static void gpu_mark_cpu_dirty_rect(const uint8_t *dest, int w, int h, int pitch)
{
	if(gpu_track_base == NULL || dest == NULL || w <= 0 || h <= 0 ||
		(const uint8_t *)dest < gpu_track_base)
	{
		return;
	}

	uintptr_t base_off = (uintptr_t)dest - (uintptr_t)gpu_track_base;
	unsigned int count = gpu_track_line_count();

	for(int row = 0; row < h; row++)
	{
		uintptr_t off = base_off + (uintptr_t)row * (uintptr_t)pitch;
		if(off >= gpu_track_bytes)
			break;
		uintptr_t endoff = off + (uintptr_t)w - 1u;
		if(endoff >= gpu_track_bytes)
			endoff = gpu_track_bytes - 1u;

		unsigned int first = (unsigned int)(off / GPU_FB_TRACK_LINE_BYTES);
		unsigned int last = (unsigned int)(endoff / GPU_FB_TRACK_LINE_BYTES);
		if(last >= count)
			last = count - 1u;

		for(unsigned int line = first; line <= last; line++)
		{
			gpu_line_set(gpu_cpu_dirty_lines, line);
			gpu_line_set(gpu_cpu_valid_lines, line);
		}
	}

	gpu_cpu_dirty = true;
}

/* Call immediately before the GPU writes the tracked framebuffer.  Publishes
 * any pending CPU-overlay pixels to SDRAM and drops them from the cache so the
 * GPU write lands on clean memory the CPU no longer holds a dirty copy of; the
 * present-time writeback can then never clobber GPU output.  Clearing the valid
 * bits forces a later CPU access on those lines to re-invalidate against the
 * GPU-produced pixels.  Mirrors Doom gpu_prepare_for_gpu_write (r_gpu.c:716-740);
 * no-op unless a CPU fallback dirtied lines this frame. */
static void gpu_prepare_for_gpu_write(void)
{
	if(!gpu_cpu_dirty || gpu_track_base == NULL)
		return;

	gpu_flush_cpu_dirty_lines();
	gpu_clear_line_bits(gpu_cpu_valid_lines);
}

static bool gpu_can_batch(uint8_t flags, int tex_width, int tex_w_mask,
	int tex_h_mask, int fb_step)
{
	if(gpu_batch_count <= 0)
		return true;

	return gpu_batch_flags == flags &&
		gpu_batch_tex_width == tex_width &&
		gpu_batch_tex_w_mask == tex_w_mask &&
		gpu_batch_tex_h_mask == tex_h_mask &&
		gpu_batch_fb_step == fb_step;
}

static bool gpu_region_within_active_frame(const uint8_t *dest, int count,
	int fb_step)
{
	if(gpu_framebuffer == NULL || dest == NULL || count <= 0 ||
		fb_step <= 0 || gpu_pitch <= 0 || gpu_height <= 0)
	{
		return false;
	}

	const uintptr_t base = (uintptr_t)gpu_framebuffer;
	const uintptr_t first = (uintptr_t)dest;
	const uintptr_t frame_bytes =
		(uintptr_t)(gpu_height - 1) * (uintptr_t)gpu_pitch +
		(uintptr_t)gpu_pitch;
	const uintptr_t end = base + frame_bytes;
	if(end < base || first < base || first >= end)
		return false;

	const uintptr_t last =
		first + (uintptr_t)(count - 1) * (uintptr_t)fb_step;
	if(last < first || last >= end)
		return false;

	if(fb_step == 1)
	{
		const uintptr_t row_off = (first - base) % (uintptr_t)gpu_pitch;
		if(row_off + (uintptr_t)count > (uintptr_t)gpu_pitch)
			return false;
	}
	return true;
}

static bool gpu_rect_within_active_frame(const uint8_t *dest, int width,
	int height, int pitch)
{
	if(gpu_framebuffer == NULL || dest == NULL || width <= 0 ||
		height <= 0 || pitch <= 0 || gpu_pitch <= 0 || gpu_height <= 0)
	{
		return false;
	}

	const uintptr_t base = (uintptr_t)gpu_framebuffer;
	const uintptr_t first = (uintptr_t)dest;
	const uintptr_t frame_bytes =
		(uintptr_t)(gpu_height - 1) * (uintptr_t)gpu_pitch +
		(uintptr_t)gpu_pitch;
	const uintptr_t end = base + frame_bytes;
	if(end < base || first < base || first >= end)
		return false;

	const uintptr_t row_off = (first - base) % (uintptr_t)gpu_pitch;
	if((uintptr_t)width > (uintptr_t)gpu_pitch ||
		row_off + (uintptr_t)width > (uintptr_t)gpu_pitch)
	{
		return false;
	}

	const uintptr_t last = first +
		(uintptr_t)(height - 1) * (uintptr_t)pitch +
		(uintptr_t)(width - 1);
	if(last < first || last >= end)
		return false;
	return true;
}

static bool gpu_add_affine(uint8_t *dest, int count, const uint8_t *source,
	int source_len, int tex_width, int tex_w_mask, int tex_h_mask,
	int sfrac, int tfrac, int sstep, int tstep, int light, int fb_step,
	uint8_t flags)
{
	if(!gpu_available || !gpu_frame_active)
		return false;
	if(dest == NULL || source == NULL || count <= 0 || count > 4095)
		return gpu_reject();
	if(light < 0 || light >= NUMCOLORMAPS)
		return gpu_reject();
	if(!gpu_is_power_of_two(tex_width) ||
		!gpu_is_power_of_two(tex_w_mask + 1) ||
		!gpu_is_power_of_two(tex_h_mask + 1))
	{
		return gpu_reject();
	}
	if(!gpu_region_within_active_frame(dest, count, fb_step))
		return gpu_reject();

	gpu_prepare_for_gpu_write();

	if(!gpu_can_batch(flags, tex_width, tex_w_mask, tex_h_mask, fb_step))
		gpu_flush_batch();

	if(gpu_batch_count == 0)
	{
		memset(&gpu_batch, 0, sizeof(gpu_batch));
		gpu_batch.flags = flags;
		gpu_batch.tex_width = (uint16_t)tex_width;
		gpu_batch.tex_w_mask = (uint16_t)tex_w_mask;
		gpu_batch.tex_h_mask = (uint16_t)tex_h_mask;
		gpu_batch.fb_step = fb_step;
		gpu_batch_flags = flags;
		gpu_batch_tex_width = tex_width;
		gpu_batch_tex_w_mask = tex_w_mask;
		gpu_batch_tex_h_mask = tex_h_mask;
		gpu_batch_fb_step = fb_step;
	}

	if(gpu_batch_count >= gpu_batch_max_lanes)
		gpu_flush_batch();

	gpu_make_source_visible(source, (uint32_t)source_len);

	unsigned lane = (unsigned)gpu_batch_count++;
	gpu_batch.fb_addr[lane] = (uint32_t)(uintptr_t)dest;
	gpu_batch.tex_addr[lane] = (uint32_t)(uintptr_t)source;
	gpu_batch.count[lane] = (uint16_t)count;
	gpu_batch.s[lane] = sfrac;
	gpu_batch.t[lane] = tfrac;
	gpu_batch.sstep[lane] = sstep;
	gpu_batch.tstep[lane] = tstep;
	gpu_batch.light[lane] = (uint8_t)light;
	gpu_batch.colormap_id[lane] = 0;

	if(gpu_batch_count >= gpu_batch_max_lanes)
		gpu_flush_batch();

	return true;
}

void OF_WolfGPU_Init(void)
{
	if(!gpu_initialized)
	{
		gpu_initialized = true;

		if(getenv("OF_ECWOLF_NOGPU") != NULL)
			return;

		const struct of_capabilities *caps = of_get_caps();
		if(caps == NULL || caps->gpu_base == 0 ||
			(caps->hw_features & OF_HW_GPU_SPAN) == 0)
		{
			return;
		}

		of_gpu_init();
		of_cache_flush();
		gpu_available = true;
		gpu_reset_source_cache();
		printf("OpenFPGA GPU: hardware command path enabled.\n");
	}

	if(!gpu_available || gpu_colormap_uploaded || NormalLight.Maps == NULL)
		return;

	of_gpu_palookup_upload(0, NormalLight.Maps, NUMCOLORMAPS * 256);
	GPU_TEX_FLUSH = 1;

	gpu_colormap_uploaded = true;
	gpu_reset_source_cache();
	printf("OpenFPGA GPU: Wolfenstein renderer acceleration enabled.\n");
}

void OF_WolfGPU_Shutdown(void)
{
	if(!gpu_available)
		return;

	OF_WolfGPU_EndFrame();
	OF_WolfGPU_ResetVideoFrames();
	of_gpu_shutdown();
	gpu_available = false;
	gpu_initialized = false;
	gpu_colormap_uploaded = false;
	gpu_reset_source_cache();
}

void OF_WolfGPU_ApplyRefreshPolicy(void)
{
	of_analogizer_state_t analogizer;
	const bool analogizerEnabled =
		of_analogizer_state(&analogizer) == 0 && analogizer.enabled;
	/* 60 Hz (Doom parity): the display consumes one swap per refresh, so the
	 * refresh rate is the present cap.  70 Hz tics on a 50 Hz panel meant a
	 * 1,1,2 tic beat every three frames; at 60 Hz the double-tic frame drops
	 * to roughly one in six and display latency shrinks. */
	const uint32_t vtotal = analogizerEnabled ?
		OF_VIDEO_VTOTAL_AUTO : OF_VIDEO_VTOTAL_60HZ;

	of_video_set_refresh_vtotal(vtotal);
}

void OF_WolfGPU_ResetVideoFrames(void)
{
	gpu_video_initialized = false;
	gpu_video_acquire_pending = false;
	gpu_video_draw_idx = -1;
	gpu_video_acquire_idx = -1;
	gpu_video_acquire_token = 0;
	gpu_video_draw_fb = NULL;
	gpu_video_last_fb = NULL;
	gpu_video_pitch = 0;
	gpu_video_height = 0;
	gpu_video_frame_bytes = 0;
	gpu_video_preserve_skip_y0 = 0;
	gpu_video_preserve_skip_y1 = 0;
	gpu_video_mode_ok_w = 0;
	gpu_video_mode_ok_h = 0;
	gpu_video_mode_ok_pitch = 0;
	gpu_video_pace_us = 0;
	gpu_video_pace_valid = false;
	gpu_video_clean_first_begin = false;
	gpu_track_base = NULL;
	gpu_track_bytes = 0;
	gpu_track_pitch = 0;
	gpu_reset_cpu_cache_tracking();
}

bool OF_WolfGPU_CanUseVideoFrames(int width, int height)
{
	OF_WolfGPU_Init();
	if(!gpu_available || width <= 0 || height <= 0)
		return false;

	/* Fast path: the hardware mode was already validated for this size.
	 * Skipping the slow path matters beyond the service-call cost --
	 * of_video_set_refresh_vtotal (via ApplyRefreshPolicy) resets the
	 * kernel's adaptive-refresh bookkeeping, so it must not run per frame. */
	if(width == gpu_video_mode_ok_w && height == gpu_video_mode_ok_h)
		return true;

	OF_WolfGPU_ApplyRefreshPolicy();

	of_video_mode_t mode;
	of_video_get_mode(&mode);

	/* The app renders into a width x height 8-bit framebuffer and presents it
	 * through the GPU page flip; that needs the ACTIVE hardware video mode to
	 * match exactly. The app forces its surface to 320x200 (c_cvars.cpp) but
	 * never sets the hardware mode -- it relies on the boot-default scaler,
	 * which an openfpgaOS resync can change. On a mismatch SDLFB falls back to
	 * a per-frame CPU software-scaling present (GPfx.Convert), which is slow
	 * for EVERYTHING including menus. So actively set the mode we need (once)
	 * before giving up. */
	if(mode.width != (uint16_t)width || mode.height != (uint16_t)height ||
		mode.color_mode != OF_VIDEO_MODE_8BIT)
	{
		static bool tried_set_mode = false;
		if(!tried_set_mode)
		{
			tried_set_mode = true;
			of_video_mode_t want;
			memset(&want, 0, sizeof(want));
			want.width = (uint16_t)width;
			want.height = (uint16_t)height;
			want.stride = 0;
			want.color_mode = OF_VIDEO_MODE_8BIT;
			of_video_set_mode(&want);
			OF_WolfGPU_ApplyRefreshPolicy();
			of_video_get_mode(&mode);
			printf("OpenFPGA GPU: set video mode %dx%d 8bit -> got %ux%u color=%u.\n",
				width, height, (unsigned)mode.width, (unsigned)mode.height,
				(unsigned)mode.color_mode);
		}
	}

	if(mode.width != (uint16_t)width || mode.height != (uint16_t)height)
	{
		if(!gpu_video_compat_logged)
		{
			printf("OpenFPGA GPU: direct present unavailable, app=%dx%d video=%ux%u.\n",
				width, height, (unsigned)mode.width, (unsigned)mode.height);
			gpu_video_compat_logged = true;
		}
		return false;
	}
	if(mode.color_mode != OF_VIDEO_MODE_8BIT)
	{
		if(!gpu_video_compat_logged)
		{
			printf("OpenFPGA GPU: direct present unavailable, video color mode=%u.\n",
				(unsigned)mode.color_mode);
			gpu_video_compat_logged = true;
		}
		return false;
	}

	gpu_video_mode_ok_w = width;
	gpu_video_mode_ok_h = height;
	gpu_video_mode_ok_pitch = mode.stride ? (int)mode.stride : (int)mode.width;
	return true;
}

void OF_WolfGPU_SetNextVideoFramePreserve(bool preserve)
{
	gpu_video_next_frame_preserve = preserve;
}

/* Rows [y0, y1) of the next frame are fully redrawn by the renderer, so the
 * acquire-time preserve copy may skip them (e.g. a full-width 3D view above
 * the status bar).  One-shot: consumed by the next buffer acquire. */
void OF_WolfGPU_SetNextVideoFramePreserveExcludeRows(int y0, int y1)
{
	gpu_video_preserve_skip_y0 = y0;
	gpu_video_preserve_skip_y1 = y1;
}

/* Microseconds elapsed since the last buffer acquire blocked on the display
 * flip (~the last vsync).  Returns false if the last acquire didn't block
 * (the game is running slower than the display), in which case the caller
 * should fall back to wall-clock sampling. */
bool OF_WolfGPU_USSinceDisplaySync(uint32_t *us_out)
{
	if(!gpu_video_pace_valid)
		return false;
	if(us_out != NULL)
		*us_out = of_time_us() - gpu_video_pace_us;
	return true;
}

bool OF_WolfGPU_FlipVideoBuffer(int idx, uint32_t *token)
{
	OF_WolfGPU_Init();
	if(!gpu_available || idx < 0)
		return false;

	const uint32_t flipToken = of_gpu_flip_to(idx);
	of_gpu_kick();
	if(token != NULL)
		*token = flipToken;
	return true;
}

static bool gpu_acquire_video_draw_buffer(int width, int height)
{
	if(gpu_video_draw_idx >= 0 && gpu_video_draw_fb != NULL)
		return true;

	const bool preserve = gpu_video_next_frame_preserve;
	int skip_y0 = gpu_video_preserve_skip_y0;
	int skip_y1 = gpu_video_preserve_skip_y1;
	gpu_video_next_frame_preserve = true;
	gpu_video_preserve_skip_y0 = 0;
	gpu_video_preserve_skip_y1 = 0;

	int draw_idx;
	if(!gpu_video_initialized)
	{
		draw_idx = of_video_acquire_next(-1, 0);
		gpu_video_initialized = true;
	}
	else if(gpu_video_acquire_pending)
	{
		/* CMD_FLIP only retires once the display consumed the previous swap
		 * (one swap per refresh -- FIFO, not mailbox), so when the game
		 * outpaces the display this fence is where the loop blocks.  Wait
		 * for it HERE with a generous bound: the kernel's own acquire wait
		 * gives up after ~5 ms and then CPU-forces the swap, which can scan
		 * out a frame the GPU is still presenting (visible hiccup/tear).
		 * fence_late counts pacing blocks; forced_swaps should stay zero. */
		const bool fence_late =
			!of_gpu_fence_reached(gpu_video_acquire_token);
		if(fence_late)
		{
			gpu_dbg_fence_late++;
			const uint32_t wait_start = of_time_us();
			while(!of_gpu_fence_reached(gpu_video_acquire_token) &&
				(uint32_t)(of_time_us() - wait_start) < 50000u)
			{
			}
			gpu_video_pace_us = of_time_us();
			gpu_video_pace_valid = true;
		}
		else
		{
			gpu_video_pace_valid = false;
		}
		draw_idx = of_video_acquire_next(gpu_video_acquire_idx,
			gpu_video_acquire_token);
		if(fence_late && !of_gpu_fence_reached(gpu_video_acquire_token))
			gpu_dbg_forced_swaps++;
		gpu_video_acquire_pending = false;
		gpu_video_acquire_idx = -1;
		gpu_video_acquire_token = 0;
	}
	else
	{
		draw_idx = of_video_acquire_next(-1, 0);
	}

	if(draw_idx < 0)
	{
		OF_WolfGPU_ResetVideoFrames();
		return false;
	}

	uint8_t *draw_fb = of_video_buffer_addr(draw_idx);
	if(draw_fb == NULL)
	{
		OF_WolfGPU_ResetVideoFrames();
		return false;
	}

	/* The hardware mode was validated by OF_WolfGPU_CanUseVideoFrames just
	 * before this call; use the cached pitch instead of re-reading the mode
	 * every frame.  Falls back to a live read if the cache is cold. */
	int pitch = gpu_video_mode_ok_pitch;
	if(width != gpu_video_mode_ok_w || height != gpu_video_mode_ok_h ||
		pitch <= 0)
	{
		of_video_mode_t mode;
		of_video_get_mode(&mode);
		pitch = mode.stride ? (int)mode.stride : (int)mode.width;
		if(mode.width != (uint16_t)width || mode.height != (uint16_t)height ||
			mode.color_mode != OF_VIDEO_MODE_8BIT || pitch <= 0)
		{
			OF_WolfGPU_ResetVideoFrames();
			return false;
		}
	}

	uint32_t frame_bytes = (uint32_t)pitch * (uint32_t)height;
	if(preserve && gpu_video_last_fb != NULL && gpu_video_last_fb != draw_fb)
	{
		/* Rows [skip_y0, skip_y1) will be fully redrawn by the renderer, so
		 * only the head (rows above the view) and tail (status bar / border
		 * rows below it) need to be carried over from the last frame. */
		if(skip_y0 < 0)
			skip_y0 = 0;
		if(skip_y1 > height)
			skip_y1 = height;
		if(skip_y0 >= skip_y1)
		{
			memcpy(draw_fb, gpu_video_last_fb, frame_bytes);
			of_cache_flush_range(draw_fb, frame_bytes);
		}
		else
		{
			const uint32_t head_bytes = (uint32_t)skip_y0 * (uint32_t)pitch;
			const uint32_t tail_off = (uint32_t)skip_y1 * (uint32_t)pitch;
			if(head_bytes != 0)
			{
				memcpy(draw_fb, gpu_video_last_fb, head_bytes);
				of_cache_flush_range(draw_fb, head_bytes);
			}
			if(tail_off < frame_bytes)
			{
				memcpy(draw_fb + tail_off, gpu_video_last_fb + tail_off,
					frame_bytes - tail_off);
				of_cache_flush_range(draw_fb + tail_off,
					frame_bytes - tail_off);
			}
		}
	}
	else if(preserve && gpu_video_last_fb == NULL)
	{
		memset(draw_fb, 0, frame_bytes);
		of_cache_flush_range(draw_fb, frame_bytes);
	}

	gpu_video_draw_idx = draw_idx;
	gpu_video_draw_fb = draw_fb;
	gpu_video_pitch = pitch;
	gpu_video_height = height;
	gpu_video_frame_bytes = frame_bytes;
	gpu_video_clean_first_begin = true;
	return true;
}

bool OF_WolfGPU_AcquireVideoFrame(uint8_t **framebuffer, int *pitch,
	int width, int height)
{
	if(framebuffer != NULL)
		*framebuffer = NULL;
	if(pitch != NULL)
		*pitch = 0;

	if(!OF_WolfGPU_CanUseVideoFrames(width, height))
		return false;
	if(!gpu_acquire_video_draw_buffer(width, height))
		return false;

	if(framebuffer != NULL)
		*framebuffer = gpu_video_draw_fb;
	if(pitch != NULL)
		*pitch = gpu_video_pitch;
	return true;
}

bool OF_WolfGPU_PresentVideoFrame(void)
{
	if(!gpu_available || gpu_video_draw_idx < 0 ||
		gpu_video_draw_fb == NULL || gpu_video_frame_bytes == 0)
	{
		return false;
	}

	const bool gpu_active = gpu_frame_active;
	if(gpu_active)
	{
		gpu_flush_batch();
		/* Publish CPU overlay writes (sprite/weapon/sky fallbacks) to SDRAM
		 * before the flip.  The GPU only runs queued draws on the kick below,
		 * so writing back the CPU-dirty lines first is race-free; drain first
		 * only if GPU work was queued after the last CPU sync (and might have
		 * been auto-flushed mid-frame).  Pure-GPU frames have no dirty lines
		 * and stay fully asynchronous. */
		if(gpu_cpu_dirty)
		{
			if(gpu_frame_dirty)
			{
				const uint32_t perfStart = OF_WolfPerf_NowUS();
				of_gpu_finish();
				OF_WolfPerf_Add(OF_WOLF_PERF_GPU_FINISH, perfStart);
			}
			gpu_flush_cpu_dirty_lines();
		}
		gpu_frame_active = false;
		gpu_frame_dirty = false;
		gpu_framebuffer = NULL;
		gpu_pitch = 0;
		gpu_height = 0;
		gpu_reset_batch();
		gpu_reset_cpu_cache_tracking();
	}
	else
	{
		of_cache_flush_range(gpu_video_draw_fb, gpu_video_frame_bytes);
	}

	uint32_t token = of_gpu_flip_to(gpu_video_draw_idx);
	of_gpu_kick();

	gpu_video_last_fb = gpu_video_draw_fb;
	gpu_video_acquire_pending = true;
	gpu_video_acquire_idx = gpu_video_draw_idx;
	gpu_video_acquire_token = token;
	gpu_video_draw_idx = -1;
	gpu_video_draw_fb = NULL;
	gpu_video_clean_first_begin = false;
	return true;
}

void OF_WolfGPU_BeginFrame(uint8_t *framebuffer, int pitch, int height)
{
	OF_WolfGPU_Init();

	if(!gpu_available || !gpu_colormap_uploaded || framebuffer == NULL ||
		pitch <= 0 || height <= 0)
	{
		return;
	}

	OF_WolfGPU_EndFrame();

	/* Texture buffers changed since the last frame: drop the GPU's internal
	 * texture cache now, while the GPU is idle (the flip fence and EndFrame
	 * above guarantee no in-flight sampling). */
	if(gpu_tex_flush_pending)
	{
		GPU_TEX_FLUSH = 1;
		gpu_tex_flush_pending = false;
	}

	gpu_framebuffer = framebuffer;
	gpu_pitch = pitch;
	gpu_height = height;
	gpu_frame_dirty = false;
	gpu_frame_active = true;
	gpu_reset_batch();

	/* Enable per-line CPU/GPU coherence tracking when rendering into a video
	 * draw buffer that fits the tracking budget; otherwise PrepareForCPUAccess
	 * falls back to a whole-frame sync.  Track against the full buffer base so
	 * view columns, the weapon and border writes all map to valid offsets. */
	if(gpu_video_draw_fb != NULL && gpu_video_frame_bytes > 0 &&
		gpu_video_frame_bytes <= GPU_FB_TRACK_MAX_BYTES &&
		((uintptr_t)gpu_video_draw_fb & (GPU_FB_TRACK_LINE_BYTES - 1u)) == 0 &&
		framebuffer >= gpu_video_draw_fb &&
		(uintptr_t)framebuffer + (uintptr_t)height * (uintptr_t)pitch <=
			(uintptr_t)gpu_video_draw_fb + gpu_video_frame_bytes)
	{
		gpu_track_base = gpu_video_draw_fb;
		gpu_track_bytes = gpu_video_frame_bytes;
		gpu_track_pitch = pitch;
	}
	else
	{
		gpu_track_base = NULL;
		gpu_track_bytes = 0;
		gpu_track_pitch = 0;
	}
	gpu_reset_cpu_cache_tracking();

	uint32_t bytes = (uint32_t)((height - 1) * pitch + pitch);
	/* Direct video acquisition leaves the draw buffer clean for the GPU:
	 * non-preserve frames have no CPU writes, and preserve copies are flushed
	 * immediately after the memcpy/memset. */
	const bool clean_video_begin =
		gpu_video_clean_first_begin && framebuffer == gpu_video_draw_fb;
	gpu_video_clean_first_begin = false;
	if(!clean_video_begin)
	{
		of_cache_flush_range(framebuffer, bytes);
	}
	of_gpu_set_framebuffer((uint32_t)(uintptr_t)framebuffer, (uint16_t)pitch);
}

void OF_WolfGPU_EndFrame(void)
{
	if(!gpu_available || !gpu_frame_active)
		return;

	gpu_flush_batch();
	const bool had_gpu_work = gpu_frame_dirty;
	if(had_gpu_work)
	{
		const uint32_t perfStart = OF_WolfPerf_NowUS();
		of_gpu_finish();
		OF_WolfPerf_Add(OF_WOLF_PERF_GPU_FINISH, perfStart);
		gpu_frame_dirty = false;
	}
	/* Write back any CPU overlay lines already tracked this frame before the
	 * invalidate drops the cache, so a full-CPU takeover after a region sync
	 * does not lose those pixels.  No-op when nothing was CPU-dirtied. */
	gpu_flush_cpu_dirty_lines();
	if(had_gpu_work)
	{
		/* Invalidate the same region the dirty-line writeback covers (the full
		 * tracked buffer when tracking is active), so flush and invalidate stay
		 * consistent and a takeover after a region sync reads GPU pixels. */
		if(gpu_track_base != NULL && gpu_track_bytes > 0)
		{
			of_cache_inval_range(gpu_track_base, gpu_track_bytes);
		}
		else if(gpu_framebuffer != NULL && gpu_pitch > 0 && gpu_height > 0)
		{
			uint32_t bytes = (uint32_t)((gpu_height - 1) * gpu_pitch + gpu_pitch);
			of_cache_inval_range(gpu_framebuffer, bytes);
		}
	}
	gpu_reset_cpu_cache_tracking();

	gpu_frame_active = false;
	gpu_frame_dirty = false;
	gpu_framebuffer = NULL;
	gpu_pitch = 0;
	gpu_height = 0;
	gpu_reset_batch();
}

void OF_WolfGPU_FallbackToCPU(void)
{
	OF_WolfGPU_EndFrame();
}

void OF_WolfGPU_PrepareForCPUAccessRect(uint8_t *dest, int width, int height,
	int pitch)
{
	if(!gpu_available || !gpu_frame_active)
		return;

	if(width <= 0 || height <= 0 || pitch <= 0)
		return;

	gpu_flush_batch();

	/* Fall back to a whole-frame sync (end the GPU frame) when fine-grained
	 * tracking is unavailable or the rect lies outside the tracked buffer:
	 * non-video-frame buffer, mismatched pitch, a frame larger than the
	 * tracking budget, or a dest below/after the tracked range. */
	const uint8_t *rect_end = dest +
		(uintptr_t)(height - 1) * (uintptr_t)pitch + (uintptr_t)width;
	if(gpu_track_base == NULL || pitch != gpu_track_pitch ||
		dest < gpu_track_base ||
		rect_end > gpu_track_base + gpu_track_bytes)
	{
		OF_WolfGPU_EndFrame();
		return;
	}

	/* Drain the GPU only when it has outstanding writes, so the CPU reads the
	 * pixels the GPU produced.  Later CPU columns in the same stage do not
	 * re-drain unless new GPU work was queued in between. */
	if(gpu_frame_dirty)
	{
		const uint32_t perfStart = OF_WolfPerf_NowUS();
		of_gpu_finish();
		OF_WolfPerf_Add(OF_WOLF_PERF_GPU_FINISH, perfStart);
		gpu_frame_dirty = false;
	}

	gpu_invalidate_rect_for_cpu(dest, width, height, pitch);
	gpu_mark_cpu_dirty_rect(dest, width, height, pitch);
}

void OF_WolfGPU_PrepareForCPUAccessColumn(uint8_t *dest, int count, int pitch)
{
	OF_WolfGPU_PrepareForCPUAccessRect(dest, 1, count, pitch);
}

bool OF_WolfGPU_IsActive(void)
{
	return gpu_available && gpu_frame_active;
}

bool OF_WolfGPU_DrawColumn(uint8_t *dest, int count, const uint8_t *source,
	int source_len, int texfrac, int texstep, int light)
{
	if(!gpu_is_power_of_two(source_len) || source_len > 256)
		return gpu_reject();

	return gpu_add_affine(dest, count, source, source_len,
		1, 0, source_len - 1,
		0, texfrac, 0, texstep, light, gpu_pitch, OF_GPU_SPAN_COLORMAP);
}

bool OF_WolfGPU_DrawMaskedColumn(uint8_t *dest, int count,
	const uint8_t *source, int source_len, int texfrac, int texstep,
	int light)
{
	if(!gpu_available || !gpu_frame_active)
		return false;
	if(dest == NULL || source == NULL || count <= 0)
		return gpu_reject();
	if(!gpu_is_power_of_two(source_len) || source_len > 256)
		return gpu_reject();

	const int texmask = source_len - 1;
	const uint8_t maskState = gpu_classify_mask_source(source, source_len);
	if(maskState == 1)
		return true;
	if(maskState == 2)
	{
		return gpu_add_affine(dest, count, source, source_len,
			1, 0, texmask, 0, texfrac, 0, texstep, light, gpu_pitch,
			OF_GPU_SPAN_COLORMAP);
	}

	int run_start = -1;
	int run_count = 0;
	int run_texfrac = 0;
	int cur_texfrac = texfrac;

	for(int i = 0; i < count; ++i, cur_texfrac += texstep)
	{
		const int sample = (cur_texfrac >> FRACBITS) & texmask;
		if(source[sample] != 0)
		{
			if(run_count == 0)
			{
				run_start = i;
				run_texfrac = cur_texfrac;
			}
			run_count++;
			continue;
		}

		if(run_count != 0)
		{
			if(!gpu_add_affine(dest + run_start * gpu_pitch, run_count,
				source, source_len, 1, 0, texmask, 0, run_texfrac,
				0, texstep, light, gpu_pitch, OF_GPU_SPAN_COLORMAP))
			{
				return false;
			}
			run_count = 0;
			run_start = -1;
		}
	}

	if(run_count != 0)
	{
		if(!gpu_add_affine(dest + run_start * gpu_pitch, run_count,
			source, source_len, 1, 0, texmask, 0, run_texfrac,
			0, texstep, light, gpu_pitch, OF_GPU_SPAN_COLORMAP))
		{
			return false;
		}
	}
	return true;
}

bool OF_WolfGPU_DrawRawColumn(uint8_t *dest, int count,
	const uint8_t *source, int source_len, int texfrac, int texstep)
{
	if(!gpu_is_power_of_two(source_len) || source_len > 1024)
		return gpu_reject();

	return gpu_add_affine(dest, count, source, source_len,
		1, 0, source_len - 1,
		0, texfrac, 0, texstep, 0, gpu_pitch, 0);
}

void OF_WolfGPU_PreloadSource(const uint8_t *source, uint32_t bytes)
{
	if(!gpu_available || source == NULL || bytes == 0)
		return;
	gpu_make_source_visible(source, bytes);
}

/* Texture pixel buffers were freed or recomposited (level precache unload,
 * palette invalidation, lazy first composition after a free).  Heap reuse can
 * place NEW texel data at an address range the source-visibility cache still
 * considers flushed -- the composited pixels then sit dirty in the CPU cache
 * while the GPU samples stale SDRAM bytes (seen as black spans on a freshly
 * switched-to weapon sprite).  Forget all cached source state so the next use
 * re-flushes, and schedule a GPU texture-cache flush for the next frame
 * start. */
void OF_WolfGPU_SourceBuffersChanged(void)
{
	if(!gpu_available)
		return;

	gpu_reset_source_cache();
	memset(gpu_mask_cache, 0, sizeof(gpu_mask_cache));
	gpu_mask_cache_last.source = NULL;
	gpu_mask_cache_last.source_len = 0;
	gpu_mask_cache_last.state = 0;
	gpu_tex_flush_pending = true;
}

bool OF_WolfGPU_DrawSpan(uint8_t *dest, int count, const uint8_t *source,
	int tex_width, int tex_height, int sfrac, int tfrac,
	int sstep, int tstep, int light)
{
	if(!gpu_is_power_of_two(tex_width) || !gpu_is_power_of_two(tex_height))
		return gpu_reject();

	return gpu_add_affine(dest, count, source, tex_width * tex_height,
		tex_width, tex_width - 1, tex_height - 1,
		sfrac, tfrac, sstep, tstep, light, 1, OF_GPU_SPAN_COLORMAP);
}

bool OF_WolfGPU_ClearSpan(uint8_t *dest, int count, uint8_t color)
{
	if(!gpu_available || !gpu_frame_active)
		return false;
	if(dest == NULL || count <= 0 || count > 65535)
		return gpu_reject();
	if(!gpu_rect_within_active_frame(dest, count, 1, gpu_pitch))
		return gpu_reject();

	gpu_prepare_for_gpu_write();
	gpu_flush_batch();
	of_gpu_clear_rect_strided((uint32_t)(uintptr_t)dest, (uint16_t)count,
		1, (uint16_t)gpu_pitch, color);
	gpu_frame_dirty = true;
	return true;
}

bool OF_WolfGPU_ClearRect(uint8_t *dest, int width, int height, uint8_t color)
{
	if(!gpu_available || !gpu_frame_active)
		return false;
	if(dest == NULL || width <= 0 || height <= 0 ||
		width > 65535 || height > 65535)
	{
		return gpu_reject();
	}
	if(!gpu_rect_within_active_frame(dest, width, height, gpu_pitch))
		return gpu_reject();

	gpu_prepare_for_gpu_write();
	gpu_flush_batch();
	of_gpu_clear_rect_strided((uint32_t)(uintptr_t)dest, (uint16_t)width,
		(uint16_t)height, (uint16_t)gpu_pitch, color);
	gpu_frame_dirty = true;
	return true;
}

#endif
