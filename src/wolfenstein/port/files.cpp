/*
** files.cpp
** Implements classes for reading from files or memory blocks
**
**---------------------------------------------------------------------------
** Copyright 1998-2008 Randy Heit
** Copyright 2005-2008 Christoph Oelckers
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
*/

#ifdef _WIN32
#define USE_WINDOWS_DWORD
#endif
#include "LzmaDec.h"

#include "files.h"
#include "filesys.h"
#include "templates.h"
#include "zdoomsupport.h"
#if defined(OF_ECWOLF_OPENFPGA)
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#endif
#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
#include "of_file.h"
#include "of_services.h"
#endif

#if defined(OF_ECWOLF_OPENFPGA)
static const long OF_ECWOLF_MAX_SLURP = 32 * 1024 * 1024;
static const long OF_ECWOLF_SLURP_CHUNK = 32 * 1024;

#if !defined(OF_PC)
static volatile int OFSlurpReadDone;
static volatile int OFSlurpReadResult;

static const char *OFSlotBasename(const char *filename)
{
	if (filename == NULL)
		return NULL;

	const char *base = filename;
	for (const char *p = filename; *p != 0; ++p)
	{
		if (*p == '/' || *p == '\\' || *p == ':')
			base = p + 1;
	}
	return base;
}

static void OFUpperCopy(char *dest, size_t destSize, const char *src)
{
	if (destSize == 0)
		return;

	size_t i = 0;
	while (i + 1 < destSize && src != NULL && src[i] != 0)
	{
		dest[i] = (char)toupper((unsigned char)src[i]);
		++i;
	}
	dest[i] = 0;
}

static long OFTryFileSizeName(const char *filename)
{
	if (filename == NULL || filename[0] == 0 ||
		OF_SVC->count <= OF_FILE_SVC_INDEX(file_size) ||
		OF_SVC->file_size == NULL)
		return -1;
	return OF_SVC->file_size(filename);
}

static int OFTrySlotFind(const char *filename, uint32_t *slot)
{
	if (filename == NULL || filename[0] == 0)
		return -1;
	return of_file_slot_find(filename, slot);
}

static int OFResolveSlotName(const char *filename, uint32_t *slot)
{
	if (OFTrySlotFind(filename, slot) == 0)
		return 0;

	const char *base = OFSlotBasename(filename);
	if (base != filename && OFTrySlotFind(base, slot) == 0)
		return 0;

	char upper[128];
	OFUpperCopy(upper, sizeof(upper), base);
	if (upper[0] != 0 && OFTrySlotFind(upper, slot) == 0)
		return 0;

	return -1;
}

static bool OFHasPrefixNoCase(const char *text, const char *prefix)
{
	if (text == NULL || prefix == NULL)
		return false;

	while (*prefix != 0)
	{
		if (*text == 0 ||
			toupper((unsigned char)*text) != toupper((unsigned char)*prefix))
		{
			return false;
		}
		++text;
		++prefix;
	}
	return true;
}

static bool OFPreferStreamSlurp(const char *filename)
{
	const char *base = OFSlotBasename(filename);
	if (base == NULL)
		return false;

	return OFHasPrefixNoCase(base, "gamemaps.") ||
		OFHasPrefixNoCase(base, "maptemp.");
}

static void OFSlurpReadCallback(int, int result)
{
	OFSlurpReadResult = result;
	OFSlurpReadDone = 1;
}

static long OFFileSizeService(const char *filename, FILE *file)
{
	if (OF_SVC == NULL || OF_SVC->magic != OF_SVC_MAGIC)
		return -1;

	long zeroLength = -1;
	long bestLength = -1;
	const char *base = OFSlotBasename(filename);
	char upper[128];
	OFUpperCopy(upper, sizeof(upper), base);

	long len = OFTryFileSizeName(upper);
	if (len > 0)
		bestLength = len;
	if (len == 0)
		zeroLength = 0;

	if (base != filename)
	{
		len = OFTryFileSizeName(base);
		if (len > 0)
		{
			if (len > bestLength)
				bestLength = len;
		}
		if (len == 0)
			zeroLength = 0;
	}

	len = OFTryFileSizeName(filename);
	if (len > 0)
	{
		if (len > bestLength)
			bestLength = len;
	}
	if (len == 0)
		zeroLength = 0;

	if (file != NULL &&
		OF_SVC->count > OF_FILE_SVC_INDEX(file_size_fd) &&
		OF_SVC->file_size_fd != NULL)
	{
		len = OF_SVC->file_size_fd(fileno(file));
		if (len > 0)
		{
			if (len > bestLength)
				bestLength = len;
		}
		if (len == 0)
			zeroLength = 0;
	}

	if (bestLength > 0)
		return bestLength;
	return zeroLength;
}

static bool OFReadSlotBlocking(uint32_t slot, uint32_t offset, void *dest, uint32_t length)
{
	unsigned char *out = (unsigned char *)dest;
	uint32_t maxRead = of_file_async_max_read();
	if (maxRead == 0 || maxRead > (uint32_t)OF_ECWOLF_SLURP_CHUNK)
		maxRead = (uint32_t)OF_ECWOLF_SLURP_CHUNK;

	while (length > 0)
	{
		uint32_t chunk = length < maxRead ? length : maxRead;
		OFSlurpReadDone = 0;
		OFSlurpReadResult = -1;

		int token = of_file_read_async((int)slot, offset, out, chunk, OFSlurpReadCallback);
		if (token < 0)
			return false;

		while (!OFSlurpReadDone)
		{
			of_file_async_poll();
			if (!of_file_async_busy())
				break;
		}
		if (!OFSlurpReadDone || OFSlurpReadResult < 0)
			return false;

		offset += chunk;
		out += chunk;
		length -= chunk;
	}

	return true;
}

static bool OFSlurpSlot(const char *filename, long knownLength, char **buffer, long *bufferLength)
{
	if (filename == NULL || knownLength <= 0 || knownLength > OF_ECWOLF_MAX_SLURP)
		return false;

	uint32_t slot = 0;
	if (OFResolveSlotName(filename, &slot) < 0)
		return false;

	char *buf = (char *)malloc((size_t)knownLength);
	if (buf == NULL)
		return false;

	if (!OFReadSlotBlocking(slot, 0, buf, (uint32_t)knownLength))
	{
		free(buf);
		return false;
	}

	*buffer = buf;
	*bufferLength = knownLength;
	return true;
}
#endif
#endif

//==========================================================================
//
// FileReader
//
// reads data from an uncompressed file or part of it
//
//==========================================================================

#if defined(OF_ECWOLF_OPENFPGA)
#define OF_SLURP_INIT , Buffer(NULL), BufferLen(0), Slurped(false)
#else
#define OF_SLURP_INIT
#endif

FileReader::FileReader ()
: File(NULL), Length(0), StartPos(0), FilePos(0) OF_SLURP_INIT, CloseOnDestruct(false)
{
}

FileReader::FileReader (const FileReader &other, long length)
: File(other.File), Length(length) OF_SLURP_INIT, CloseOnDestruct(false)
{
	FilePos = StartPos = ftell (other.File);
}

FileReader::FileReader (const char *filename)
: File(NULL), Length(0), StartPos(0), FilePos(0) OF_SLURP_INIT, CloseOnDestruct(false)
{
	if (!Open(filename))
	{
		I_Error ("Could not open %s", filename);
	}
}

FileReader::FileReader (FILE *file)
: File(file), Length(0), StartPos(0), FilePos(0) OF_SLURP_INIT, CloseOnDestruct(false)
{
	Length = CalcFileLen();
}

FileReader::FileReader (FILE *file, long length)
: File(file), Length(length) OF_SLURP_INIT, CloseOnDestruct(true)
{
	FilePos = StartPos = ftell (file);
}

#undef OF_SLURP_INIT

FileReader::~FileReader ()
{
#if defined(OF_ECWOLF_OPENFPGA)
	if (Slurped && Buffer != NULL)
	{
		free (Buffer);
		Buffer = NULL;
	}
#endif
	if (CloseOnDestruct && File != NULL)
	{
		fclose (File);
		File = NULL;
	}
}

bool FileReader::Open (const char *filename)
{
	File = ::File(filename).open("rb");
	if (File == NULL) return false;
	FilePos = 0;
	StartPos = 0;
	CloseOnDestruct = true;
#if defined(OF_ECWOLF_OPENFPGA)
	// Game data files are read with scattered Seek()/Read() calls. On the
	// OpenFPGA target those files are deferred APF data slots, so make each
	// FileReader an in-memory reader up front. Prefer the direct slot DMA path
	// when the OS exposes an exact size; keep a sequential stdio slurp as a
	// fallback for older kernels and non-slot files.
	long knownLength = -1;
#if !defined(OF_PC)
	knownLength = OFFileSizeService(filename, File);
	if (OFPreferStreamSlurp(filename))
		knownLength = -1;
#endif
	if (SlurpStream(filename, knownLength))
	{
		fclose (File);
		File = NULL;
		CloseOnDestruct = false;
		return true;
	}

	fclose(File);
	File = ::File(filename).open("rb");
	if (File == NULL)
		return false;
	FilePos = 0;
	StartPos = 0;
	CloseOnDestruct = true;
	// Slurp failed (empty file or read error): fall back to streaming so we are
	// never worse than before. Log it: on this target streaming means scattered
	// reads may be slow or broken, so a game-data file landing here is the thing
	// to investigate.
	printf ("FileReader: NOT slurped, streaming fallback: %s\n", filename);
#endif
	Length = CalcFileLen();
	return true;
}

#if defined(OF_ECWOLF_OPENFPGA)
bool FileReader::SlurpStream (const char *filename, long knownLength)
{
#if !defined(OF_PC)
	char *slotBuffer = NULL;
	long slotLength = 0;
	if (!OFPreferStreamSlurp(filename) &&
		OFSlurpSlot(filename, knownLength, &slotBuffer, &slotLength))
	{
		Buffer = slotBuffer;
		BufferLen = slotLength;
		Length = slotLength;
		FilePos = 0;
		StartPos = 0;
		Slurped = true;
		return true;
	}
#endif

	// Read the whole stream start-to-EOF into RAM. Do not call fseek() before
	// this: a fresh FILE is already at offset 0, and some APF-backed streams
	// enter a bad state after even a rewind probe. Also avoid realloc(); grow
	// via small chunks, then copy once into the final buffer.

	struct SlurpChunk { char *raw; char *data; long len; SlurpChunk *next; };
	SlurpChunk *head = NULL, *tail = NULL;
	long total = 0;
	bool ok = true;

	for (;;)
	{
		char *raw = (char *)malloc ((size_t)OF_ECWOLF_SLURP_CHUNK + 511);
		if (raw == NULL) { ok = false; break; }
		char *cd = (char *)(((uintptr_t)raw + 511u) & ~(uintptr_t)511u);
		long got = (long)fread (cd, 1, (size_t)OF_ECWOLF_SLURP_CHUNK, File);
		if (got <= 0) { free (raw); if (ferror (File)) ok = false; break; }

		SlurpChunk *c = (SlurpChunk *)malloc (sizeof (SlurpChunk));
		if (c == NULL) { free (raw); ok = false; break; }
		c->raw = raw; c->data = cd; c->len = got; c->next = NULL;
		if (tail) tail->next = c; else head = c;
		tail = c;

		total += got;
		if (total > OF_ECWOLF_MAX_SLURP) { ok = false; break; }
		if (knownLength > 0 && total >= knownLength) break;
		if (feof (File)) break;
	}

	if (knownLength > 0 && total != knownLength)
		ok = false;

	char *buf = (ok && total > 0) ? (char *)malloc ((size_t)total) : NULL;
	if (buf != NULL)
	{
		long pos = 0;
		for (SlurpChunk *c = head; c != NULL; c = c->next)
		{
			memcpy (buf + pos, c->data, (size_t)c->len);
			pos += c->len;
		}
	}

	for (SlurpChunk *c = head; c != NULL; )         // free the chunk chain
	{
		SlurpChunk *n = c->next;
		free (c->raw);
		free (c);
		c = n;
	}

	if (buf == NULL)
		return false;

	Buffer = buf;
	BufferLen = total;
	Length = total;
	FilePos = 0;
	StartPos = 0;
	Slurped = true;
	return true;
}
#endif


void FileReader::ResetFilePtr ()
{
	FilePos = ftell (File);
}

long FileReader::Tell () const
{
	return FilePos - StartPos;
}

long FileReader::Seek (long offset, int origin)
{
	if (origin == SEEK_SET)
	{
		offset += StartPos;
	}
	else if (origin == SEEK_CUR)
	{
		offset += FilePos;
	}
	else if (origin == SEEK_END)
	{
		offset += StartPos + Length;
	}
#if defined(OF_ECWOLF_OPENFPGA)
	if (Slurped)
	{
		// In-RAM seek (StartPos is 0 when slurped); clamp like MemoryReader.
		if (offset < StartPos) offset = StartPos;
		else if (offset > StartPos + Length) offset = StartPos + Length;
		FilePos = offset;
		return 0;
	}
#endif
	if (0 == fseek (File, offset, SEEK_SET))
	{
		FilePos = offset;
		return 0;
	}
	return -1;
}

long FileReader::Read (void *buffer, long len)
{
	assert(len >= 0);
	if (len <= 0) return 0;
	if (FilePos + len > StartPos + Length)
	{
		len = Length - FilePos + StartPos;
	}
#if defined(OF_ECWOLF_OPENFPGA)
	if (Slurped)
	{
		if (len <= 0) return 0;
		memcpy (buffer, Buffer + FilePos, len);
		FilePos += len;
		return len;
	}
#endif
	len = (long)fread (buffer, 1, len, File);
	FilePos += len;
	return len;
}

char *FileReader::Gets(char *strbuf, int len)
{
#if defined(OF_ECWOLF_OPENFPGA)
	if (Slurped)
		return GetsFromBuffer(Buffer, strbuf, len);
#endif
	if (len <= 0 || FilePos >= StartPos + Length) return NULL;
	char *p = fgets(strbuf, len, File);
	if (p != NULL)
	{
		int old = FilePos;
		FilePos = ftell(File);
		if (FilePos - StartPos > Length)
		{
			strbuf[Length - old + StartPos] = 0;
		}
	}
	return p;
}

char *FileReader::GetsFromBuffer(const char * bufptr, char *strbuf, int len)
{
	if (len>Length-FilePos) len=Length-FilePos;
	if (len <= 0) return NULL;

	char *p = strbuf;
	while (len > 1)
	{
		if (bufptr[FilePos] == 0)
		{
			FilePos++;
			break;
		}
		if (bufptr[FilePos] != '\r')
		{
			*p++ = bufptr[FilePos];
			len--;
			if (bufptr[FilePos] == '\n') 
			{
				FilePos++;
				break;
			}
		}
		FilePos++;
	}
	if (p==strbuf) return NULL;
	*p++=0;
	return strbuf;
}

long FileReader::CalcFileLen() const
{
#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
	long serviceLen = OFFileSizeService(NULL, File);
	if (serviceLen > 0)
		return serviceLen;
#endif
	long endpos;

	fseek (File, 0, SEEK_END);
	endpos = ftell (File);
	fseek (File, 0, SEEK_SET);
	return endpos;
}

//==========================================================================
//
// FileReaderZ
//
// The zlib wrapper
// reads data from a ZLib compressed stream
//
//==========================================================================

FileReaderZ::FileReaderZ (FileReader &file, bool zip)
: File(file), SawEOF(false)
{
	int err;

	FillBuffer ();

	Stream.zalloc = Z_NULL;
	Stream.zfree = Z_NULL;

	if (!zip) err = inflateInit (&Stream);
	else err = inflateInit2 (&Stream, -MAX_WBITS);

	if (err != Z_OK)
	{
		I_Error ("FileReaderZ: inflateInit failed: %d\n", err);
	}
}

FileReaderZ::~FileReaderZ ()
{
	inflateEnd (&Stream);
}

long FileReaderZ::Read (void *buffer, long len)
{
	int err;

	Stream.next_out = (Bytef *)buffer;
	Stream.avail_out = len;

	do
	{
		err = inflate (&Stream, Z_SYNC_FLUSH);
		if (Stream.avail_in == 0 && !SawEOF)
		{
			FillBuffer ();
		}
	} while (err == Z_OK && Stream.avail_out != 0);

	if (err != Z_OK && err != Z_STREAM_END)
	{
		I_Error ("Corrupt zlib stream");
	}

	if (Stream.avail_out != 0)
	{
		I_Error ("Ran out of data in zlib stream");
	}

	return len - Stream.avail_out;
}

void FileReaderZ::FillBuffer ()
{
	long numread = File.Read (InBuff, BUFF_SIZE);

	if (numread < BUFF_SIZE)
	{
		SawEOF = true;
	}
	Stream.next_in = InBuff;
	Stream.avail_in = numread;
}

//==========================================================================
//
// FileReaderZ
//
// The bzip2 wrapper
// reads data from a libbzip2 compressed stream
//
//==========================================================================

FileReaderBZ2::FileReaderBZ2 (FileReader &file)
: File(file), SawEOF(false)
{
	int err;

	FillBuffer ();

	Stream.bzalloc = NULL;
	Stream.bzfree = NULL;
	Stream.opaque = NULL;

	err = BZ2_bzDecompressInit(&Stream, 0, 0);

	if (err != BZ_OK)
	{
		I_Error ("FileReaderBZ2: bzDecompressInit failed: %d\n", err);
	}
}

FileReaderBZ2::~FileReaderBZ2 ()
{
	BZ2_bzDecompressEnd (&Stream);
}

long FileReaderBZ2::Read (void *buffer, long len)
{
	int err;

	Stream.next_out = (char *)buffer;
	Stream.avail_out = len;

	do
	{
		err = BZ2_bzDecompress(&Stream);
		if (Stream.avail_in == 0 && !SawEOF)
		{
			FillBuffer ();
		}
	} while (err == BZ_OK && Stream.avail_out != 0);

	if (err != BZ_OK && err != BZ_STREAM_END)
	{
		I_Error ("Corrupt bzip2 stream");
	}

	if (Stream.avail_out != 0)
	{
		I_Error ("Ran out of data in bzip2 stream");
	}

	return len - Stream.avail_out;
}

void FileReaderBZ2::FillBuffer ()
{
	long numread = File.Read(InBuff, BUFF_SIZE);

	if (numread < BUFF_SIZE)
	{
		SawEOF = true;
	}
	Stream.next_in = (char *)InBuff;
	Stream.avail_in = numread;
}

//==========================================================================
//
// bz_internal_error
//
// libbzip2 wants this, since we build it with BZ_NO_STDIO set.
//
//==========================================================================

extern "C" void bz_internal_error (int errcode)
{
	I_FatalError("libbzip2: internal error number %d\n", errcode);
}

//==========================================================================
//
// FileReaderLZMA
//
// The lzma wrapper
// reads data from a LZMA compressed stream
//
//==========================================================================

// This is retarded but necessary to work around the inclusion of windows.h in recent
// LZMA versions, meaning it's no longer possible to include the LZMA headers in files.h. 
// As a result we cannot declare the CLzmaDec member in the header so we work around
// it my wrapping it into another struct that can be declared anonymously in the header.
struct FileReaderLZMA::StreamPointer
{
	CLzmaDec Stream;
};

static void *SzAlloc(void *, size_t size) { return malloc(size); }
static void SzFree(void *, void *address) { free(address); }
ISzAlloc g_Alloc = { SzAlloc, SzFree };

FileReaderLZMA::FileReaderLZMA (FileReader &file, size_t uncompressed_size, bool zip)
: File(file), SawEOF(false)
{
	BYTE header[4 + LZMA_PROPS_SIZE];
	int err;

	assert(zip == true);

	Size = uncompressed_size;
	OutProcessed = 0;

	// Read zip LZMA properties header
	if (File.Read(header, sizeof(header)) < (long)sizeof(header))
	{
		I_Error("FileReaderLZMA: File too shart\n");
	}
	if (header[2] + header[3] * 256 != LZMA_PROPS_SIZE)
	{
		I_Error("FileReaderLZMA: LZMA props size is %d (expected %d)\n",
			header[2] + header[3] * 256, LZMA_PROPS_SIZE);
	}

	FillBuffer();

	Streamp = new StreamPointer;
	LzmaDec_Construct(&Streamp->Stream);
	err = LzmaDec_Allocate(&Streamp->Stream, header + 4, LZMA_PROPS_SIZE, &g_Alloc);

	if (err != SZ_OK)
	{
		I_Error("FileReaderLZMA: LzmaDec_Allocate failed: %d\n", err);
	}

	LzmaDec_Init(&Streamp->Stream);
}

FileReaderLZMA::~FileReaderLZMA ()
{
	LzmaDec_Free(&Streamp->Stream, &g_Alloc);
	delete Streamp;
}

long FileReaderLZMA::Read (void *buffer, long len)
{
	int err;
	Byte *next_out = (Byte *)buffer;

	do
	{
		ELzmaFinishMode finish_mode = LZMA_FINISH_ANY;
		ELzmaStatus status;
		size_t out_processed = len;
		size_t in_processed = InSize;

		err = LzmaDec_DecodeToBuf(&Streamp->Stream, next_out, &out_processed, InBuff + InPos, &in_processed, finish_mode, &status);
		InPos += in_processed;
		InSize -= in_processed;
		next_out += out_processed;
		len = (long)(len - out_processed);
		if (err != SZ_OK)
		{
			I_Error ("Corrupt LZMA stream");
		}
		if (in_processed == 0 && out_processed == 0)
		{
			if (status != LZMA_STATUS_FINISHED_WITH_MARK)
			{
				I_Error ("Corrupt LZMA stream");
			}
		}
		if (InSize == 0 && !SawEOF)
		{
			FillBuffer ();
		}
	} while (err == SZ_OK && len != 0);

	if (err != Z_OK && err != Z_STREAM_END)
	{
		I_Error ("Corrupt LZMA stream");
	}

	if (len != 0)
	{
		I_Error ("Ran out of data in LZMA stream");
	}

	return (long)(next_out - (Byte *)buffer);
}

void FileReaderLZMA::FillBuffer ()
{
	long numread = File.Read(InBuff, BUFF_SIZE);

	if (numread < BUFF_SIZE)
	{
		SawEOF = true;
	}
	InPos = 0;
	InSize = numread;
}

//==========================================================================
//
// MemoryReader
//
// reads data from a block of memory
//
//==========================================================================

MemoryReader::MemoryReader (const char *buffer, long length)
{
	bufptr=buffer;
	Length=length;
	FilePos=0;
}

MemoryReader::~MemoryReader ()
{
}

long MemoryReader::Tell () const
{
	return FilePos;
}

long MemoryReader::Seek (long offset, int origin)
{
	switch (origin)
	{
	case SEEK_CUR:
		offset+=FilePos;
		break;

	case SEEK_END:
		offset+=Length;
		break;

	}
	FilePos=clamp<long>(offset,0,Length);
	return 0;
}

long MemoryReader::Read (void *buffer, long len)
{
	if (len>Length-FilePos) len=Length-FilePos;
	if (len<0) len=0;
	memcpy(buffer,bufptr+FilePos,len);
	FilePos+=len;
	return len;
}

char *MemoryReader::Gets(char *strbuf, int len)
{
	return GetsFromBuffer(bufptr, strbuf, len);
}
