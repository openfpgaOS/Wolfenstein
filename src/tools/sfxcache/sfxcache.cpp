/*
** sfxcache -- pre-render Wolfenstein AdLib sound effects at native OPL quality
**
** Renders every AdLib sound chunk found in AUDIOT files through the Nuked
** OPL3 emulator (cycle-accurate YMF262 die emulation, run in OPL2 mode) at
** the chip's native rate (49716 Hz), band-limits it down to the Pocket
** mixer's 48000 Hz with a windowed-sinc filter, and writes a single
** hash-keyed pack file.  DBOPL renders each sound in parallel purely for
** LOUDNESS CALIBRATION: the on-device cache-miss fallback synthesizes with
** DBOPL, so the pack is gain-matched to it and hits/misses sound identical
** in level.  The pack is injected into wolfmidi.zip as the
** "sfxcache.ofx" lump (see scripts/sfxcache.sh); the port looks sounds up by
** FNV-1a hash of the AdLib script bytes and falls back to on-device DBOPL
** synthesis on a miss, so the cache is always optional and never stale.
**
** Usage: sfxcache <out.ofx> <AUDIOHED> <AUDIOT> [<AUDIOHED> <AUDIOT> ...]
**
** Pack format (all little-endian):
**   u32 magic "OFX1"   u32 count   u32 rate   u32 reserved
**   count * { u64 hash; u32 byteOffset; u32 sampleCount; }
**   16-bit signed mono PCM blobs
*/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>

#include "dbopl.h"
extern "C" {
#include "opl3.h"
}
#include "fmopl.h"

static const unsigned OPL_NATIVE_RATE = 49716;  // YM3812 sample rate
static const unsigned OUT_RATE        = 48000;  // Pocket mixer native rate
static const unsigned TICK_RATE       = 140;    // AdLib SFX service rate
static const unsigned RELEASE_TICKS   = 35;     // 1/4 s key-off tail

/* OPL register bases (match port/id_sd.h) */
#define alChar    0x20
#define alScale   0x40
#define alAttack  0x60
#define alSus     0x80
#define alWave    0xE0
#define alFreqL   0xA0
#define alFreqH   0xB0
#define alFeedCon 0xC0

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t fnv1a64(const uint8_t *data, size_t n)
{
	uint64_t h = 14695981039346656037ull;
	for(size_t i = 0; i < n; ++i)
	{
		h ^= data[i];
		h *= 1099511628211ull;
	}
	return h;
}

static std::vector<uint8_t> readFile(const char *path)
{
	std::vector<uint8_t> out;
	FILE *f = fopen(path, "rb");
	if(!f)
	{
		fprintf(stderr, "sfxcache: cannot open %s\n", path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize((size_t)len);
	if(len > 0 && fread(out.data(), 1, (size_t)len, f) != (size_t)len)
	{
		fprintf(stderr, "sfxcache: short read on %s\n", path);
		exit(1);
	}
	fclose(f);
	return out;
}

/* Render one AdLib script through DBOPL at the chip's native rate.
 * Mirrors the port's SD_PrepareAdLibSound()/SDL_AlSetChanInst() exactly
 * (instrument on channel 0, 140 Hz freq/key service, x4 loudness). */
static std::vector<int16_t> renderNative(DBOPL::Chip &chip,
                                         const uint8_t *raw, uint32_t length)
{
	chip.SetVolume(MAX_VOLUME);
	for(Bit32u r = 0x20; r <= 0xF5; r++)
		chip.WriteReg(r, 0);
	chip.WriteReg(1, 0x20);  // Set WSE=1

	/* Instrument: raw[6..21]; modulator/carrier cells of channel 0. */
	const uint8_t *inst = raw + 6;
	const Bit32u m = 0, c = 3;
	chip.WriteReg(m + alChar,   inst[0]);
	chip.WriteReg(m + alScale,  inst[2]);
	chip.WriteReg(m + alAttack, inst[4]);
	chip.WriteReg(m + alSus,    inst[6]);
	chip.WriteReg(m + alWave,   inst[8]);
	chip.WriteReg(c + alChar,   inst[1]);
	chip.WriteReg(c + alScale,  inst[3]);
	chip.WriteReg(c + alAttack, inst[5]);
	chip.WriteReg(c + alSus,    inst[7]);
	chip.WriteReg(c + alWave,   inst[9]);
	chip.WriteReg(0 + alFreqL,   0);
	chip.WriteReg(0 + alFreqH,   0);
	chip.WriteReg(0 + alFeedCon, 0);

	const uint8_t block = ((raw[22] & 7) << 2) | 0x20;
	const uint8_t *data = raw + 23;

	const uint32_t totalTicks = length + RELEASE_TICKS;
	std::vector<int16_t> pcm;
	pcm.reserve((size_t)totalTicks * (OPL_NATIVE_RATE / TICK_RATE + 1));

	Bit32s block32[512];
	uint32_t sampleAcc = 0;
	for(uint32_t tick = 0; tick < totalTicks; ++tick)
	{
		if(tick < length)
		{
			if(data[tick])
			{
				chip.WriteReg(alFreqL, data[tick]);
				chip.WriteReg(alFreqH, block);
			}
			else
				chip.WriteReg(alFreqH, 0);
		}
		else if(tick == length)
			chip.WriteReg(alFreqH, 0);

		sampleAcc += OPL_NATIVE_RATE;
		uint32_t tickSamples = sampleAcc / TICK_RATE;
		sampleAcc -= tickSamples * TICK_RATE;

		while(tickSamples > 0)
		{
			const uint32_t n = tickSamples > 512 ? 512 : tickSamples;
			chip.GenerateBlock2(n, block32);
			for(uint32_t i = 0; i < n; ++i)
			{
				/* x4 to match the loudness of the port's music path. */
				Bit32s s = block32[i] << 2;
				if(s > 32767) s = 32767;
				else if(s < -32768) s = -32768;
				pcm.push_back((int16_t)s);
			}
			tickSamples -= n;
		}
	}
	return pcm;
}

/* Render the same script through Nuked OPL3 (OPL2 compatibility mode --
 * register 0x105 is never written, so the chip stays "old", which also
 * forces both output channels on regardless of the OPL3 pan bits).  Raw
 * chip-level output; gain calibration against DBOPL happens in main(). */
static std::vector<int16_t> renderNativeNuked(const uint8_t *raw,
                                              uint32_t length)
{
	static opl3_chip chip;
	OPL3_Reset(&chip, OPL_NATIVE_RATE);

	for(uint16_t r = 0x20; r <= 0xF5; r++)
		OPL3_WriteReg(&chip, r, 0);
	OPL3_WriteReg(&chip, 0x01, 0x20);  // WSE (OPL2 etiquette; harmless here)

	const uint8_t *inst = raw + 6;
	const uint16_t m = 0, c = 3;
	OPL3_WriteReg(&chip, m + alChar,   inst[0]);
	OPL3_WriteReg(&chip, m + alScale,  inst[2]);
	OPL3_WriteReg(&chip, m + alAttack, inst[4]);
	OPL3_WriteReg(&chip, m + alSus,    inst[6]);
	OPL3_WriteReg(&chip, m + alWave,   inst[8]);
	OPL3_WriteReg(&chip, c + alChar,   inst[1]);
	OPL3_WriteReg(&chip, c + alScale,  inst[3]);
	OPL3_WriteReg(&chip, c + alAttack, inst[5]);
	OPL3_WriteReg(&chip, c + alSus,    inst[7]);
	OPL3_WriteReg(&chip, c + alWave,   inst[9]);
	OPL3_WriteReg(&chip, 0 + alFreqL,   0);
	OPL3_WriteReg(&chip, 0 + alFreqH,   0);
	OPL3_WriteReg(&chip, 0 + alFeedCon, 0);

	const uint8_t block = ((raw[22] & 7) << 2) | 0x20;
	const uint8_t *data = raw + 23;

	const uint32_t totalTicks = length + RELEASE_TICKS;
	std::vector<int16_t> pcm;
	pcm.reserve((size_t)totalTicks * (OPL_NATIVE_RATE / TICK_RATE + 1));

	uint32_t sampleAcc = 0;
	for(uint32_t tick = 0; tick < totalTicks; ++tick)
	{
		if(tick < length)
		{
			if(data[tick])
			{
				OPL3_WriteReg(&chip, alFreqL, data[tick]);
				OPL3_WriteReg(&chip, alFreqH, block);
			}
			else
				OPL3_WriteReg(&chip, alFreqH, 0);
		}
		else if(tick == length)
			OPL3_WriteReg(&chip, alFreqH, 0);

		sampleAcc += OPL_NATIVE_RATE;
		uint32_t tickSamples = sampleAcc / TICK_RATE;
		sampleAcc -= tickSamples * TICK_RATE;

		while(tickSamples-- > 0)
		{
			int16_t frame[2];
			OPL3_GenerateResampled(&chip, frame);
			pcm.push_back(frame[0]);   // mono: OPL2 mode mirrors L/R
		}
	}
	return pcm;
}

/* Render through MAME fmopl -- a TRUE YM3812 (OPL2) emulation, unlike
 * DBOPL/Nuked which are OPL3 models in compatibility mode.  Selected with
 * SFX_EMU=fmopl; serves as the architectural tiebreaker. */
static std::vector<int16_t> renderNativeFmopl(const uint8_t *raw,
                                              uint32_t length)
{
	static bool inited = false;
	if(!inited)
	{
		YM3812Init(1, 3579545, OPL_NATIVE_RATE);
		inited = true;
	}
	const int vol = MAX_VOLUME;
	for(int r = 0x20; r <= 0xF5; r++)
		YM3812Write(0, r, 0, vol);
	YM3812Write(0, 0x01, 0x20, vol);

	const uint8_t *inst = raw + 6;
	const int m = 0, c = 3;
	YM3812Write(0, m + alChar,   inst[0], vol);
	YM3812Write(0, m + alScale,  inst[2], vol);
	YM3812Write(0, m + alAttack, inst[4], vol);
	YM3812Write(0, m + alSus,    inst[6], vol);
	YM3812Write(0, m + alWave,   inst[8], vol);
	YM3812Write(0, c + alChar,   inst[1], vol);
	YM3812Write(0, c + alScale,  inst[3], vol);
	YM3812Write(0, c + alAttack, inst[5], vol);
	YM3812Write(0, c + alSus,    inst[7], vol);
	YM3812Write(0, c + alWave,   inst[9], vol);
	YM3812Write(0, 0 + alFreqL,   0, vol);
	YM3812Write(0, 0 + alFreqH,   0, vol);
	YM3812Write(0, 0 + alFeedCon, 0, vol);

	const uint8_t block = ((raw[22] & 7) << 2) | 0x20;
	const uint8_t *data = raw + 23;

	const uint32_t totalTicks = length + RELEASE_TICKS;
	std::vector<int16_t> pcm;
	pcm.reserve((size_t)totalTicks * (OPL_NATIVE_RATE / TICK_RATE + 1));

	int16_t block16[512];
	uint32_t sampleAcc = 0;
	for(uint32_t tick = 0; tick < totalTicks; ++tick)
	{
		if(tick < length)
		{
			if(data[tick])
			{
				YM3812Write(0, alFreqL, data[tick], vol);
				YM3812Write(0, alFreqH, block, vol);
			}
			else
				YM3812Write(0, alFreqH, 0, vol);
		}
		else if(tick == length)
			YM3812Write(0, alFreqH, 0, vol);

		sampleAcc += OPL_NATIVE_RATE;
		uint32_t tickSamples = sampleAcc / TICK_RATE;
		sampleAcc -= tickSamples * TICK_RATE;

		while(tickSamples > 0)
		{
			const uint32_t n = tickSamples > 512 ? 512 : tickSamples;
			YM3812UpdateOne(0, block16, (int)n);
			for(uint32_t i = 0; i < n; ++i)
				pcm.push_back(block16[i]);
			tickSamples -= n;
		}
	}
	return pcm;
}

/* Band-limited 49716 -> 48000 conversion: 33-tap windowed-sinc evaluated
 * per output sample (offline, so no polyphase table needed). */
static std::vector<int16_t> resample(const std::vector<int16_t> &in)
{
	const double ratio = (double)OPL_NATIVE_RATE / (double)OUT_RATE;
	/* Downsampling: cut off just below the OUTPUT Nyquist, expressed as a
	 * fraction of the input rate. */
	const double cutoff = 0.5 / ratio * 0.95;
	const int taps = 16;  /* one-sided; 33-tap kernel */

	const size_t outCount = (size_t)((double)in.size() / ratio);
	std::vector<int16_t> out;
	out.reserve(outCount);

	for(size_t n = 0; n < outCount; ++n)
	{
		const double center = (double)n * ratio;
		const long ci = (long)floor(center + 0.5);
		double acc = 0.0, norm = 0.0;
		for(long k = ci - taps; k <= ci + taps; ++k)
		{
			const double x = (double)k - center;
			/* sinc low-pass */
			double s = (x == 0.0) ? 2.0 * cutoff
				: sin(2.0 * M_PI * cutoff * x) / (M_PI * x);
			/* Blackman window */
			const double w = 0.42
				+ 0.5 * cos(M_PI * x / (double)taps)
				+ 0.08 * cos(2.0 * M_PI * x / (double)taps);
			s *= (fabs(x) <= (double)taps) ? w : 0.0;
			const double v = (k >= 0 && k < (long)in.size()) ? (double)in[k] : 0.0;
			acc += v * s;
			norm += s;
		}
		/* Normalize by the kernel sum for exact unity passband gain. */
		double y = acc / (norm > 1e-9 ? norm : 1.0);
		if(y > 32767.0) y = 32767.0;
		else if(y < -32768.0) y = -32768.0;
		out.push_back((int16_t)lrint(y));
	}
	return out;
}

/* Model the AdLib card's analog output stage: the YM3014 DAC fed an op-amp
 * with real low-pass rolloff, and the chip's raw edges (authentic, verified
 * by two independent emulators agreeing to r=0.995) sound harsher on modern
 * gear than any 1992 setup ever did.  2nd-order Butterworth; default 12 kHz,
 * SFX_LPF_HZ overrides (0 disables for the chip-raw purist render). */
static void lowpass(std::vector<int16_t> &pcm, double hz)
{
	if(hz <= 0.0 || hz >= OUT_RATE / 2.0)
		return;
	const double w = tan(M_PI * hz / OUT_RATE);
	const double k = 1.0 / (1.0 + sqrt(2.0) * w + w * w);
	const double b0 = w * w * k, b1 = 2.0 * b0, b2 = b0;
	const double a1 = 2.0 * (w * w - 1.0) * k;
	const double a2 = (1.0 - sqrt(2.0) * w + w * w) * k;

	double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
	for(size_t i = 0; i < pcm.size(); ++i)
	{
		const double x = pcm[i];
		double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		x2 = x1; x1 = x;
		y2 = y1; y1 = y;
		if(y > 32767.0) y = 32767.0;
		else if(y < -32768.0) y = -32768.0;
		pcm[i] = (int16_t)lrint(y);
	}
}

struct Entry
{
	std::vector<int16_t> pcm;
};

int main(int argc, char **argv)
{
	if(argc < 4 || ((argc - 2) % 2) != 0)
	{
		fprintf(stderr,
			"usage: sfxcache <out.ofx> <AUDIOHED> <AUDIOT> [<AUDIOHED> <AUDIOT> ...]\n");
		return 1;
	}

	double lpfHz = 12000.0;
	if(getenv("SFX_LPF_HZ"))
		lpfHz = atof(getenv("SFX_LPF_HZ"));

	DBOPL::Chip *chip = new DBOPL::Chip();
	chip->Setup(OPL_NATIVE_RATE);

	std::map<uint64_t, Entry> entries;
	unsigned rendered = 0, skipped = 0;
	int peakDbopl = 0, peakNuked = 0;

	for(int a = 2; a + 1 < argc; a += 2)
	{
		std::vector<uint8_t> hed = readFile(argv[a]);
		std::vector<uint8_t> aud = readFile(argv[a + 1]);
		const size_t numOff = hed.size() / 4;
		if(numOff < 2)
			continue;

		unsigned fileRendered = 0;
		for(size_t i = 0; i + 1 < numOff; ++i)
		{
			const uint32_t off = rd32(&hed[i * 4]);
			const uint32_t end = rd32(&hed[(i + 1) * 4]);
			if(end <= off || end > aud.size())
				continue;
			const uint32_t size = end - off;
			/* Structural validation of an AdLib sound chunk: 4-byte length,
			 * 2-byte priority, 16-byte instrument, block, data[length]. */
			if(size < 24 || size > 0x10000)
				continue;
			const uint8_t *raw = &aud[off];
			const uint32_t length = rd32(raw);
			if(length == 0 || length > size - 23)
				continue;
			if(!(raw[12] | raw[13]))  /* mSus | cSus -- same check as the game */
				continue;

			const uint64_t hash = fnv1a64(raw, 23 + length);
			if(entries.count(hash))
			{
				skipped++;
				continue;
			}

			Entry e;
			/* Reference render (the on-device fallback emulator) -- used
			 * only to calibrate the pack's loudness. */
			std::vector<int16_t> reference = renderNative(*chip, raw, length);
			for(size_t k = 0; k < reference.size(); ++k)
			{
				const int a = abs((int)reference[k]);
				if(a > peakDbopl) peakDbopl = a;
			}

			std::vector<int16_t> native = renderNativeNuked(raw, length);
			for(size_t k = 0; k < native.size(); ++k)
			{
				const int a = abs((int)native[k]);
				if(a > peakNuked) peakNuked = a;
			}

			/* SFX_EMIT_REFERENCE=1: emit the DBOPL reference render
			 * instead of Nuked (for emulator A/B comparisons). */
			const char *emu = getenv("SFX_EMU");
			if(emu && strcmp(emu, "fmopl") == 0)
				native = renderNativeFmopl(raw, length);
			e.pcm = resample(getenv("SFX_EMIT_REFERENCE") ? reference : native);
			lowpass(e.pcm, lpfHz);

			/* Trim the silent part of the release tail. */
			const size_t minSamples =
				(size_t)((uint64_t)length * OUT_RATE / TICK_RATE);
			size_t samples = e.pcm.size();
			while(samples > minSamples && abs((int)e.pcm[samples - 1]) < 16)
				--samples;
			e.pcm.resize(samples);

			entries[hash] = e;
			rendered++;
			fileRendered++;
		}
		printf("sfxcache: %s -> %u AdLib sounds\n", argv[a + 1], fileRendered);
	}

	/* Gain-match the Nuked output to the DBOPL fallback so cache hits and
	 * misses play at the same level on device. */
	double gain = 1.0;
	if(!getenv("SFX_EMIT_REFERENCE") && peakNuked > 0 && peakDbopl > 0)
		gain = (double)peakDbopl / (double)peakNuked;
	if(gain < 0.25) gain = 0.25;
	else if(gain > 16.0) gain = 16.0;
	printf("sfxcache: loudness calibration: dbopl peak %d, nuked peak %d, gain %.3f\n",
		peakDbopl, peakNuked, gain);
	for(std::map<uint64_t, Entry>::iterator it = entries.begin();
	    it != entries.end(); ++it)
	{
		std::vector<int16_t> &pcm = it->second.pcm;
		for(size_t k = 0; k < pcm.size(); ++k)
		{
			double v = pcm[k] * gain;
			if(v > 32767.0) v = 32767.0;
			else if(v < -32768.0) v = -32768.0;
			pcm[k] = (int16_t)lrint(v);
		}
	}

	/* Write the pack. */
	FILE *f = fopen(argv[1], "wb");
	if(!f)
	{
		fprintf(stderr, "sfxcache: cannot write %s\n", argv[1]);
		return 1;
	}
	const uint32_t count = (uint32_t)entries.size();
	uint32_t header[4] = { 0, count, OUT_RATE, 0 };
	memcpy(&header[0], "OFX1", 4);
	fwrite(header, 1, 16, f);

	uint32_t offset = 16 + count * 16;
	for(std::map<uint64_t, Entry>::const_iterator it = entries.begin();
	    it != entries.end(); ++it)
	{
		const uint64_t hash = it->first;
		const uint32_t samples = (uint32_t)it->second.pcm.size();
		fwrite(&hash, 1, 8, f);
		fwrite(&offset, 1, 4, f);
		fwrite(&samples, 1, 4, f);
		offset += samples * 2;
	}
	for(std::map<uint64_t, Entry>::const_iterator it = entries.begin();
	    it != entries.end(); ++it)
		fwrite(it->second.pcm.data(), 2, it->second.pcm.size(), f);
	fclose(f);

	printf("sfxcache: wrote %s (%u sounds, %u duplicates skipped, %.1f KB)\n",
		argv[1], count, skipped, offset / 1024.0);
	return 0;
}
