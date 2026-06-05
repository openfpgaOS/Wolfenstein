/*
** file_gamemaps.cpp
**
**---------------------------------------------------------------------------
** Copyright 2011 Braden Obrzut
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
**
*/

#include "filesys.h"
#include "doomerrors.h"
#include "wl_def.h"
#include "resourcefile.h"
#include "tmemory.h"
#include "w_wad.h"
#include "m_swap.h"
#include "zstring.h"
#include "wolfmapcommon.h"
#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
#include "of_file.h"
#endif

struct FMapLump;

class FGamemaps : public FResourceFile
{
	public:
		FGamemaps(const char* filename, FileReader *file);
		~FGamemaps();

		FResourceLump *GetLump(int lump);
		bool Open(bool quiet);

	private:
		FMapLump* Lumps;

		TUniquePtr<FileReader> mapheadReader;
		// Gamemaps = Carmack+RLEW, Maptemp = RLEW
		bool carmacked;
		char *directData;

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
		bool ReplaceReaderFromSlot(const DWORD *offsets, unsigned int mapCount);
#endif
};

FGamemaps::FGamemaps(const char* filename, FileReader *file) : FResourceFile(filename, file), Lumps(NULL), mapheadReader(NULL), directData(NULL)
{
	FString path(filename);
	int lastSlash = path.LastIndexOfAny("/\\:");
	int lastDot = path.LastIndexOf('.');
	FString extension = path.Mid(lastDot+1);

	carmacked = path.Mid(lastSlash+1, 7).CompareNoCase("maptemp") != 0;

	path = path.Left(lastSlash+1);

	FString mapheadFile = FString("maphead.") + extension;
	if(Wads.CheckIfWadLoaded(path.Left(lastSlash)) == -1)
	{
		File directory(path.Len() > 0 ? path : ".");
		mapheadFile = path + directory.getInsensitiveFile(mapheadFile, true);

		mapheadReader = new FileReader();
		if(!mapheadReader->Open(mapheadFile))
			mapheadReader.Reset();
	}
	else // Embedded vanilla data?
	{
		FLumpReader *lreader = reinterpret_cast<FLumpReader *>(file);

		for(DWORD i = 0; i < lreader->LumpOwner()->LumpCount(); ++i)
		{
			FResourceLump *lump = lreader->LumpOwner()->GetLump(i);
			if(lump->FullName.CompareNoCase(mapheadFile) == 0)
			{
				mapheadReader = lump->NewReader();
				break;
			}
		}
	}

	if(!mapheadReader)
	{
		FString error;
		error.Format("Could not open gamemaps since %s is missing.", mapheadFile.GetChars());
		throw CRecoverableError(error);
	}
}

FGamemaps::~FGamemaps()
{
	if(Lumps != NULL)
		delete[] Lumps;
	delete[] directData;
}

FResourceLump *FGamemaps::GetLump(int lump)
{
	return &Lumps[lump];
}

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
static volatile int ofGamemapsReadDone;
static volatile int ofGamemapsReadResult;

static void OFGamemapsReadCallback(int, int result)
{
	ofGamemapsReadResult = result;
	ofGamemapsReadDone = 1;
}

static const char *OFGamemapsBaseName(const char *filename)
{
	const char *base = filename;
	for(const char *p = filename; p != NULL && *p != 0; ++p)
	{
		if(*p == '/' || *p == '\\' || *p == ':')
			base = p + 1;
	}
	return base;
}

static void OFGamemapsUpperCopy(char *dest, size_t destSize, const char *src)
{
	if(destSize == 0)
		return;

	size_t i = 0;
	while(i + 1 < destSize && src != NULL && src[i] != 0)
	{
		dest[i] = (char)toupper((unsigned char)src[i]);
		++i;
	}
	dest[i] = 0;
}

static bool OFGamemapsResolveSlot(const char *filename, uint32_t &slot)
{
	if(filename == NULL)
		return false;
	if(of_file_slot_find(filename, &slot) == 0)
		return true;

	const char *base = OFGamemapsBaseName(filename);
	if(base != filename && of_file_slot_find(base, &slot) == 0)
		return true;

	char upper[128];
	OFGamemapsUpperCopy(upper, sizeof(upper), base);
	return upper[0] != 0 && of_file_slot_find(upper, &slot) == 0;
}

static bool OFGamemapsReadSlot(uint32_t slot, uint32_t offset, void *dest,
	uint32_t length)
{
	unsigned char *out = (unsigned char *)dest;
	uint32_t maxRead = of_file_async_max_read();
	if(maxRead == 0 || maxRead > 32768u)
		maxRead = 32768u;

	while(length > 0)
	{
		uint32_t chunk = length < maxRead ? length : maxRead;
		ofGamemapsReadDone = 0;
		ofGamemapsReadResult = -1;

		int token = of_file_read_async((int)slot, offset, out, chunk,
			OFGamemapsReadCallback);
		if(token < 0)
			return false;

		while(!ofGamemapsReadDone)
		{
			of_file_async_poll();
			if(!of_file_async_busy())
				break;
		}
		if(!ofGamemapsReadDone || ofGamemapsReadResult < 0)
			return false;

		offset += chunk;
		out += chunk;
		length -= chunk;
	}
	return true;
}

bool FGamemaps::ReplaceReaderFromSlot(const DWORD *offsets, unsigned int mapCount)
{
	static const uint32_t headerSize = PLANES * 6 + 20;
	static const uint32_t maxDirectBytes = 4u * 1024u * 1024u;

	uint32_t slot = 0;
	if(!OFGamemapsResolveSlot(Filename, slot))
		return false;

	BYTE *header = new BYTE[headerSize];
	uint32_t maxEnd = 0;
	bool sawValidHeader = false;

	for(unsigned int i = 0;i < mapCount;i++)
	{
		const DWORD headerOffset = offsets[i];
		if(headerOffset == 0 || headerOffset == 0xFFFFFFFFu ||
			headerOffset > maxDirectBytes - headerSize)
		{
			continue;
		}

		if(!OFGamemapsReadSlot(slot, headerOffset, header, headerSize))
			continue;

		const WORD width = ReadLittleShort(&header[PLANES*6]);
		const WORD height = ReadLittleShort(&header[PLANES*6+2]);
		if(width == 0 || height == 0 || width > 1024 || height > 1024)
			continue;

		uint32_t mapEnd = headerOffset + headerSize;
		for(unsigned int j = 0;j < PLANES;j++)
		{
			const DWORD planeOffset = ReadLittleLong(&header[4*j]);
			const WORD planeLength = ReadLittleShort(&header[PLANES*4+2*j]);
			if(planeLength == 0)
				continue;
			const uint32_t planeEnd = planeOffset + (uint32_t)planeLength;
			if(planeEnd < planeOffset || planeEnd > maxDirectBytes)
			{
				delete[] header;
				return false;
			}
			if(planeEnd > mapEnd)
				mapEnd = planeEnd;
		}

		if(mapEnd > maxEnd)
			maxEnd = mapEnd;
		sawValidHeader = true;
	}

	delete[] header;
	if(!sawValidHeader || maxEnd == 0 || maxEnd > maxDirectBytes)
		return false;

	char *buffer = new char[maxEnd];
	if(!OFGamemapsReadSlot(slot, 0, buffer, maxEnd))
	{
		delete[] buffer;
		return false;
	}

	delete Reader;
	Reader = new MemoryReader(buffer, maxEnd);
	delete[] directData;
	directData = buffer;
	return true;
}
#endif

bool FGamemaps::Open(bool quiet)
{
	WORD rlewTag;

	// Read the map head.
	// First two bytes is the tag for the run length encoding
	// Followed by offsets in the gamemaps file, we'll count until we
	// hit a 0 offset.
	unsigned int NumPossibleMaps = (mapheadReader->GetLength()-2)/4;
	mapheadReader->Seek(0, SEEK_SET);
	DWORD* offsets = new DWORD[NumPossibleMaps];
	mapheadReader->Read(&rlewTag, 2);
	rlewTag = LittleShort(rlewTag);
	mapheadReader->Read(offsets, NumPossibleMaps*4);
	for(NumLumps = 0;NumLumps < NumPossibleMaps;++NumLumps)
	{
		offsets[NumLumps] = LittleLong(offsets[NumLumps]);
		if(offsets[NumLumps] == 0 || offsets[NumLumps] == 0xFFFFFFFFu)
			break;
	}

	// We allocate 2 lumps per map so...
	static const unsigned int NUM_MAP_LUMPS = 2;
	const unsigned int NumMaps = NumLumps;

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
	ReplaceReaderFromSlot(offsets, NumMaps);
#endif

	NumLumps *= NUM_MAP_LUMPS;

	Lumps = new FMapLump[NumLumps];
	// The slot-backed FileReader must read map headers into a HEAP buffer: on
	// the OpenFPGA target a Seek to a non-zero offset followed by a read into a
	// stack buffer returns garbage (every working engine read targets the
	// heap). The garbage previously corrupted Width/Height, yielding a
	// multi-hundred-MB LumpSize and an out-of-memory abort in FillCache.
	BYTE *header = new BYTE[PLANES*6+20];
	int badMaps = 0;
	unsigned int firstBadMap = 0;
	long firstBadOffset = 0;
	long firstBadRead = 0;
	WORD firstBadWidth = 0;
	WORD firstBadHeight = 0;
	DWORD firstBadPlaneEnd = 0;
	const long readerLength = Reader->GetLength();
	for(unsigned int i = 0;i < NumLumps/NUM_MAP_LUMPS;++i)
	{
		// Map marker
		FMapLump &markerLump = Lumps[i*NUM_MAP_LUMPS];
		// Hey we don't need to use a temporary name here!
		// First map is MAP01 and so forth.
		char lumpname[14];
		mysnprintf(lumpname, 14, "MAP%02d", i+1);
		markerLump.Owner = this;
		markerLump.LumpNameSetup(lumpname);
		markerLump.Namespace = ns_global;
		markerLump.LumpSize = 0;

		// Make the data lump
		FMapLump &dataLump = Lumps[i*NUM_MAP_LUMPS+1];
		dataLump.rlewTag = rlewTag;
		dataLump.carmackCompressed = carmacked;
		Reader->Seek(offsets[i], SEEK_SET);
		memset(header, 0, PLANES*6+20);
		long headerRead = Reader->Read(header, PLANES*6+20);

		dataLump.Owner = this;
		dataLump.LumpNameSetup("PLANES");
		dataLump.Namespace = ns_global;
		for(unsigned int j = 0;j < PLANES;j++)
		{
			dataLump.Header.PlaneOffset[j] = ReadLittleLong(&header[4*j]);
			dataLump.Header.PlaneLength[j] = ReadLittleShort(&header[PLANES*4+2*j]);
		}
		dataLump.Header.Width = ReadLittleShort(&header[PLANES*6]);
		dataLump.Header.Height = ReadLittleShort(&header[PLANES*6+2]);
		memcpy(dataLump.Header.Name, &header[PLANES*6+4], 16);

		bool planeRangesValid = true;
		DWORD maxPlaneEnd = 0;
		for(unsigned int j = 0;j < PLANES;j++)
		{
			if(dataLump.Header.PlaneLength[j] == 0)
				continue;

			const DWORD planeEnd = dataLump.Header.PlaneOffset[j] +
				(DWORD)dataLump.Header.PlaneLength[j];
			if(planeEnd < dataLump.Header.PlaneOffset[j] ||
				(readerLength > 0 && planeEnd > (DWORD)readerLength))
			{
				planeRangesValid = false;
			}
			if(planeEnd > maxPlaneEnd)
				maxPlaneEnd = planeEnd;
		}

		// Guard against a corrupt/short read producing an absurd allocation
		// later in FillCache: if the dimensions are not sane, mark the lump
		// empty (LumpSize 0) so FillCache skips it instead of allocating garbage.
		if(headerRead != PLANES*6+20 ||
			dataLump.Header.Width == 0 || dataLump.Header.Height == 0 ||
			dataLump.Header.Width > 1024 || dataLump.Header.Height > 1024 ||
			!planeRangesValid)
		{
			markerLump.Namespace = ns_hidden;
			dataLump.Namespace = ns_hidden;
			dataLump.LumpSize = 0;
			++badMaps;
			if(firstBadMap == 0)
			{
				firstBadMap = i + 1;
				firstBadOffset = (long)offsets[i];
				firstBadRead = headerRead;
				firstBadWidth = dataLump.Header.Width;
				firstBadHeight = dataLump.Header.Height;
				firstBadPlaneEnd = maxPlaneEnd;
			}
		}
		else
			dataLump.LumpSize += dataLump.Header.Width*dataLump.Header.Height*PLANES*2;
	}
	{
		static bool gamemapsLogged = false;
		if(!gamemapsLogged && (badMaps > 0
#if defined(OF_ECWOLF_OPENFPGA) && defined(OF_ECWOLF_STARTUP_PROFILE)
			|| true
#endif
			))
		{
			gamemapsLogged = true;
			Printf("gamemaps: readerLen=%ld mapheadLen=%ld maps=%u badMaps=%d",
				(long)Reader->GetLength(), (long)mapheadReader->GetLength(),
				NumLumps/NUM_MAP_LUMPS, badMaps);
			if(firstBadMap != 0)
			{
				Printf(" firstBad=MAP%02u offset=%ld read=%ld wh=%ux%u planeEnd=%lu",
					firstBadMap, firstBadOffset, firstBadRead,
					(unsigned)firstBadWidth, (unsigned)firstBadHeight,
					(unsigned long)firstBadPlaneEnd);
			}
			Printf("\n");
		}
	}
	delete[] header;
	delete[] offsets;
	if(!quiet) Printf(", %d lumps\n", NumLumps);
	return true;
}

FResourceFile *CheckGamemaps(const char *filename, FileReader *file, bool quiet)
{
	FString fname(filename);
	int lastSlash = fname.LastIndexOfAny("/\\:");
	if(lastSlash != -1)
		fname = fname.Mid(lastSlash+1, 8);
	else
		fname = fname.Left(8);

	// File must be gamemaps.something or maptemp.something
	if(fname.Len() == 8 && (fname.CompareNoCase("gamemaps") == 0 || fname.Left(7).CompareNoCase("maptemp") == 0))
	{
		FResourceFile *rf = new FGamemaps(filename, file);
		if(rf->Open(quiet)) return rf;
		rf->Reader = NULL; // to avoid destruction of reader
		delete rf;
	}
	return NULL;
}
