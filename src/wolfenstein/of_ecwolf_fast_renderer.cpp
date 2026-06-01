#include "of_ecwolf_fast_renderer.h"

#if defined(OF_ECWOLF_FAST_RENDERER) && defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actor.h"
#include "gamemap.h"
#include "id_ca.h"
#include "of_ecwolf_gpu.h"
#include "r_data/colormaps.h"
#include "thingdef/thingdef.h"
#include "textures/textures.h"
#include "wl_agent.h"
#include "wl_draw.h"
#include "wl_game.h"
#include "wl_main.h"
#include "wl_play.h"
#include "wl_shade.h"
#include "wl_state.h"

extern int viewshift;
extern fixed viewz;
extern angle_t viewangle;
extern short midangle;
extern short focaltx, focalty, viewtx, viewty;
extern int min_wallheight;
extern fixed gLevelVisibility;
extern fixed gLevelMaxLightVis;
extern int gLevelLight;

static const int FAST_TEXTUREBASE = 0x4000000;
static const int FAST_MAX_TEXTURE_SIZE = 256;
static const fixed FAST_MINDIST = 0x4000l;

struct FastTexture
{
	FTextureID id;
	FTexture *texture;
	const byte *pixels;
	int width;
	int height;
	int texxscale;
	int texyscale;
	unsigned frame;
	bool valid;
	bool checked;
};

enum FastColumnResult
{
	FastColumnMiss,
	FastColumnOccluded,
	FastColumnDraw
};

struct FastWallColumn
{
	const FastTexture *texture;
	const byte *source;
	int postx;
	int topRow;
	int count;
	int texfrac;
	int texstep;
	int shadeIndex;
};

static const int FAST_MAX_COLUMNS = 640;

static const GameMap *fast_map;
static MapSpot fast_plane_map;
static MapSpot *fast_spots;
static unsigned fast_width;
static unsigned fast_height;
static bool fast_map_checked;
static bool fast_map_supported;
static bool fast_logged_enabled;
static FString fast_fallback_reason;
static unsigned fast_pitch;

static FastTexture *fast_textures;
static int fast_texture_count;
static unsigned fast_texture_frame = 1;
static FastWallColumn fast_wall_columns[FAST_MAX_COLUMNS];

static bool fast_is_power_of_two(int value)
{
	return value > 0 && (value & (value - 1)) == 0;
}

static MapSpot fast_spot(int x, int y)
{
	if((unsigned)x >= fast_width || (unsigned)y >= fast_height)
		return NULL;
	return fast_spots[(unsigned)y * fast_width + (unsigned)x];
}

static void fast_set_fallback(const char *reason)
{
	if(fast_fallback_reason.IsEmpty())
		fast_fallback_reason = reason;
}

void OF_WolfFastRenderer_Reset(void)
{
	delete[] fast_spots;
	fast_spots = NULL;
	fast_width = 0;
	fast_height = 0;
	fast_map = NULL;
	fast_plane_map = NULL;
	fast_map_checked = false;
	fast_map_supported = false;
	fast_logged_enabled = false;
	fast_fallback_reason = "";

	delete[] fast_textures;
	fast_textures = NULL;
	fast_texture_count = 0;
	fast_texture_frame = 1;
}

static FastTexture *fast_texture_slot(FTextureID id)
{
	const int index = id.GetIndex();
	if(index <= 0 || index >= fast_texture_count)
		return NULL;
	return &fast_textures[index];
}

static bool fast_prepare_texture(FTextureID id, FastTexture **out)
{
	if(out != NULL)
		*out = NULL;
	if(!id.isValid())
		return false;

	FastTexture *slot = fast_texture_slot(id);
	if(slot == NULL)
		return false;

	if(slot->checked && slot->frame == fast_texture_frame)
	{
		if(out != NULL)
			*out = slot;
		return slot->valid;
	}

	slot->id = id;
	slot->texture = TexMan(id);
	slot->pixels = NULL;
	slot->width = 0;
	slot->height = 0;
	slot->texxscale = 0;
	slot->texyscale = 0;
	slot->frame = fast_texture_frame;
	slot->valid = false;
	slot->checked = true;

	FTexture *texture = slot->texture;
	if(texture == NULL)
		return false;

	const int width = texture->GetWidth();
	const int height = texture->GetHeight();
	if(width <= 0 || height <= 0 ||
		width > FAST_MAX_TEXTURE_SIZE || height > FAST_MAX_TEXTURE_SIZE ||
		!fast_is_power_of_two(width) || !fast_is_power_of_two(height))
	{
		return false;
	}
	if(texture->xScale <= 0 || texture->yScale <= 0)
		return false;

	const int texxscale = FAST_TEXTUREBASE / texture->xScale;
	const int texyscale = texture->yScale >> (FRACBITS - 8);
	if(texxscale <= 0 || texyscale <= 0)
		return false;
	if((texyscale >> 2) != height)
		return false;

	const byte *pixels = texture->GetPixels();
	if(pixels == NULL)
		return false;
	OF_WolfGPU_PreloadSource(pixels, (uint32_t)(width * height));

	slot->pixels = pixels;
	slot->width = width;
	slot->height = height;
	slot->texxscale = texxscale;
	slot->texyscale = texyscale;
	slot->valid = true;

	if(out != NULL)
		*out = slot;
	return true;
}

static bool fast_validate_spot_textures(MapSpot spot)
{
	if(spot == NULL || spot->tile == NULL)
		return true;

	for(unsigned side = 0; side < 4; ++side)
	{
		FastTexture *texture;
		if(!fast_prepare_texture(spot->GetTexture(side), &texture) ||
			texture == NULL)
		{
			fast_set_fallback("unsupported wall texture");
			return false;
		}
	}
	return true;
}

static bool fast_build_map_cache(void)
{
	if(getenv("OF_ECWOLF_NOFAST") != NULL)
	{
		fast_set_fallback("disabled by OF_ECWOLF_NOFAST");
		return false;
	}
	if(map == NULL || map->NumPlanes() == 0)
	{
		fast_set_fallback("no active map");
		return false;
	}

	const GameMap::Header &header = map->GetHeader();
	if(header.width == 0 || header.height == 0)
	{
		fast_set_fallback("empty map");
		return false;
	}
	if(header.tileSize != 64)
	{
		fast_set_fallback("non-64 map tile size");
		return false;
	}

	const unsigned count = header.width * header.height;
	MapSpot *spots = new MapSpot[count];
	if(spots == NULL)
	{
		fast_set_fallback("map cache allocation failed");
		return false;
	}

	delete[] fast_spots;
	fast_spots = spots;
	fast_plane_map = map->GetPlane(0).map;
	fast_width = header.width;
	fast_height = header.height;

	delete[] fast_textures;
	fast_texture_count = TexMan.NumTextures() + 1;
	fast_textures = new FastTexture[fast_texture_count];
	if(fast_textures == NULL)
	{
		fast_set_fallback("texture cache allocation failed");
		return false;
	}
	memset(fast_textures, 0, sizeof(FastTexture) * fast_texture_count);
	fast_texture_frame++;
	if(fast_texture_frame == 0)
		fast_texture_frame = 1;

	for(unsigned y = 0; y < fast_height; ++y)
	{
		for(unsigned x = 0; x < fast_width; ++x)
		{
			MapSpot spot = map->GetSpot(x, y, 0);
			fast_spots[y * fast_width + x] = spot;
			if(!fast_validate_spot_textures(spot))
				return false;
		}
	}

	return true;
}

static bool fast_ensure_map_cache(void)
{
	MapSpot planeMap = NULL;
	if(map != NULL && map->NumPlanes() > 0)
		planeMap = map->GetPlane(0).map;

	if(fast_map == map && fast_plane_map == planeMap && fast_map_checked)
		return fast_map_supported;

	OF_WolfFastRenderer_Reset();
	fast_map = map;
	fast_plane_map = planeMap;
	fast_map_checked = true;
	fast_fallback_reason = "";
	fast_map_supported = fast_build_map_cache();
	if(fast_map_supported)
	{
		fast_logged_enabled = true;
		printf("OpenFPGA fast renderer: enabled for %ux%u map.\n",
			fast_width, fast_height);
	}
	else
	{
		printf("OpenFPGA fast renderer: fallback (%s).\n",
			fast_fallback_reason.IsEmpty() ? "unsupported map" :
				fast_fallback_reason.GetChars());
	}
	return fast_map_supported;
}

static int fast_slide_texture_offset(unsigned int style, int intercept,
	int amount)
{
	if(!amount)
		return 0;

	switch(style)
	{
		default:
			return -amount;
		case SLIDE_Split:
			return intercept < FRACUNIT / 2 ? amount / 2 : -amount / 2;
		case SLIDE_Invert:
			return amount;
	}
}

static MapTile::Side fast_hit_dir(bool vertical, int xtilestep, int ytilestep)
{
	if(vertical)
		return xtilestep == -1 ? MapTile::East : MapTile::West;
	return ytilestep == -1 ? MapTile::South : MapTile::North;
}

static int fast_calc_height(int32_t xintercept, int32_t yintercept)
{
	fixed z = FixedMul(xintercept - viewx, viewcos) -
		FixedMul(yintercept - viewy, viewsin);
	if(z < FAST_MINDIST)
		z = FAST_MINDIST;
	const int height = (heightnumerator << 8) / z;
	if(height < min_wallheight)
		min_wallheight = height;
	return height;
}

static bool fast_calc_column_draw(int postx, int wall_height,
	const FastTexture *texture, int *top_row, int *count,
	int *texfrac, int *texstep)
{
	int ywcount = wall_height;
	int yd = wall_height;
	if(yd <= 0)
		yd = 100;

	const int topoffset = ywcount *
		((viewz + fixed(map->GetPlane(0).depth << FRACBITS)) >> 8) /
		(32 << (FRACBITS - 5));
	const int botoffset = ywcount * (viewz >> 8) /
		(32 << (FRACBITS - 5));

	int topRow = viewheight / 2 - topoffset - viewshift;
	if(topRow < 0)
		topRow = 0;

	int bottomRow = viewheight / 2 - botoffset - 1 - viewshift;
	int yw = (texture->texyscale >> 2) - 1;

	while(bottomRow >= viewheight)
	{
		ywcount -= texture->texyscale;
		while(ywcount <= 0)
		{
			ywcount += yd;
			yw--;
		}
		bottomRow--;
	}
	if(yw < 0)
		yw = (texture->texyscale >> 2) -
			((-yw) % (texture->texyscale >> 2));
	if(bottomRow < topRow)
		return false;

	*top_row = topRow;
	*count = bottomRow - topRow + 1;
	*texstep = int(((int64_t)texture->texyscale << FRACBITS) / yd);
	const int texfracBottom = ((yw + 1) << FRACBITS) - 1;
	*texfrac = texfracBottom - (*texstep * (*count - 1));
	(void)postx;
	return *count > 0;
}

static void fast_cpu_draw_column(byte *dest, int postx,
	const FastTexture *texture, const byte *source, int topRow, int count,
	int texfrac, int texstep, int shadeIndex)
{
	const byte *curshades = &NormalLight.Maps[shadeIndex << 8];
	const int bottomRow = topRow + count - 1;

	byte *out = dest + topRow * (int)fast_pitch + postx;
	for(int y = topRow; y <= bottomRow; ++y)
	{
		int sample = texfrac >> FRACBITS;
		if((unsigned)sample >= (unsigned)texture->height)
			sample &= texture->height - 1;
		*out = curshades[source[sample]];
		out += fast_pitch;
		texfrac += texstep;
	}
}

static FastColumnResult fast_trace_wall_column(int postx,
	int32_t xintercept, int32_t yintercept, MapSpot spot,
	bool vertical, int xtilestep, int ytilestep, int xtile, int ytile,
	int texdelta, FastWallColumn *column)
{
	if(spot == NULL || spot->tile == NULL)
		return FastColumnMiss;

	MapTile::Side hitdir = fast_hit_dir(vertical, xtilestep, ytilestep);
	spot->amFlags |= AM_Visible;

	int textureCoord;
	if(vertical)
	{
		textureCoord = (yintercept + texdelta +
			fast_slide_texture_offset(spot->slideStyle, (word)yintercept,
				spot->slideAmount[hitdir])) & (FRACUNIT - 1);
		if(xtilestep == -1 && !spot->tile->offsetVertical)
			xintercept += TILEGLOBAL,
			textureCoord = (FRACUNIT - textureCoord) & (FRACUNIT - 1);
	}
	else
	{
		textureCoord = (xintercept + texdelta +
			fast_slide_texture_offset(spot->slideStyle, (word)xintercept,
				spot->slideAmount[hitdir])) & (FRACUNIT - 1);
		if(!spot->tile->offsetHorizontal)
		{
			if(ytilestep == -1)
				yintercept += TILEGLOBAL;
			else
				textureCoord = (FRACUNIT - textureCoord) & (FRACUNIT - 1);
		}
	}

	FTextureID textureId = spot->GetTexture(hitdir);
	MapSpot adj = NULL;
	if(vertical)
	{
		const int ax = xtilestep == -1 ? xtile + 1 : xtile - 1;
		adj = fast_spot(ax, ytile);
		if(adj && adj->tile && adj->tile->offsetHorizontal &&
			!adj->tile->offsetVertical)
		{
			textureId = adj->GetTexture(hitdir);
		}
	}
	else
	{
		const int ay = ytilestep == -1 ? ytile + 1 : ytile - 1;
		adj = fast_spot(xtile, ay);
		if(adj && adj->tile && adj->tile->offsetVertical &&
			!adj->tile->offsetHorizontal)
		{
			textureId = adj->GetTexture(hitdir);
		}
	}

	FastTexture *texture = NULL;
	if(!fast_prepare_texture(textureId, &texture) || texture == NULL)
		return FastColumnMiss;

	const int wall_height = fast_calc_height(xintercept, yintercept);
	wallheight[postx] = wall_height;

	textureCoord -= textureCoord % texture->texxscale;
	int texColumn = textureCoord / texture->texxscale;
	if((unsigned)texColumn >= (unsigned)texture->width)
		texColumn &= texture->width - 1;
	const byte *source = texture->pixels + texColumn * texture->height;

	const int shade = LIGHT2SHADE(gLevelLight + r_extralight);
	const int tz = FixedMul(r_depthvisibility << 8, wall_height);
	const int shadeIndex = GETPALOOKUP(MAX(tz, MINZ), shade);

	int topRow;
	int count;
	int texstep;
	int texfrac;
	if(!fast_calc_column_draw(postx, wall_height, texture, &topRow, &count,
		&texfrac, &texstep))
	{
		return FastColumnOccluded;
	}

	if(column == NULL)
		return FastColumnMiss;

	column->texture = texture;
	column->source = source;
	column->postx = postx;
	column->topRow = topRow;
	column->count = count;
	column->texfrac = texfrac;
	column->texstep = texstep;
	column->shadeIndex = shadeIndex;
	return FastColumnDraw;
}

static void fast_submit_wall_column(byte *framebuffer,
	const FastWallColumn &column)
{
	byte *dest = framebuffer + column.topRow * (int)fast_pitch + column.postx;
	if(OF_WolfGPU_DrawColumn(dest, column.count, column.source,
		column.texture->height, column.texfrac, column.texstep,
		column.shadeIndex))
	{
		return;
	}

	OF_WolfGPU_PrepareForCPUAccessColumn(dest, column.count, (int)fast_pitch);
	fast_cpu_draw_column(framebuffer, column.postx, column.texture,
		column.source, column.topRow, column.count, column.texfrac,
		column.texstep, column.shadeIndex);
}

static bool fast_trace_columns()
{
	static word xspot[2], yspot[2];
	int32_t xstep = 0, ystep = 0;
	longword xpartial = 0, ypartial = 0;
	MapSpot focalspot = fast_spot(focaltx, focalty);
	const bool playerInPushwallBackTile =
		focalspot != NULL && focalspot->pushAmount != 0;

	for(int pixx = 0; pixx < viewwidth; ++pixx)
	{
		fast_wall_columns[pixx].count = 0;

		short xtilestep = 0;
		short ytilestep = 0;

		short angl = midangle + pixelangle[pixx];
		if(angl < 0)
			angl += FINEANGLES;
		if(angl >= ANG360)
			angl -= FINEANGLES;

		if(angl < ANG90)
		{
			xtilestep = 1;
			ytilestep = -1;
			xstep = finetangent[ANG90 - 1 - angl];
			ystep = -finetangent[angl];
			xpartial = (viewx & (TILEGLOBAL - 1)) == 0 ?
				TILEGLOBAL : TILEGLOBAL - (viewx & (TILEGLOBAL - 1));
			ypartial = viewy & (TILEGLOBAL - 1);
		}
		else if(angl < ANG180)
		{
			xtilestep = -1;
			ytilestep = -1;
			xstep = -finetangent[angl - ANG90];
			ystep = -finetangent[ANG180 - 1 - angl];
			xpartial = viewx & (TILEGLOBAL - 1);
			ypartial = viewy & (TILEGLOBAL - 1);
		}
		else if(angl < ANG270)
		{
			xtilestep = -1;
			ytilestep = 1;
			xstep = -finetangent[ANG270 - 1 - angl];
			ystep = finetangent[angl - ANG180];
			xpartial = viewx & (TILEGLOBAL - 1);
			ypartial = (viewy & (TILEGLOBAL - 1)) == 0 ?
				TILEGLOBAL : TILEGLOBAL - (viewy & (TILEGLOBAL - 1));
		}
		else
		{
			xtilestep = 1;
			ytilestep = 1;
			xstep = finetangent[angl - ANG270];
			ystep = finetangent[ANG360 - 1 - angl];
			xpartial = (viewx & (TILEGLOBAL - 1)) == 0 ?
				TILEGLOBAL : TILEGLOBAL - (viewx & (TILEGLOBAL - 1));
			ypartial = (viewy & (TILEGLOBAL - 1)) == 0 ?
				TILEGLOBAL : TILEGLOBAL - (viewy & (TILEGLOBAL - 1));
		}

		int32_t yintercept = FixedMul(ystep, xpartial) + viewy;
		short xtile = focaltx + xtilestep;
		xspot[0] = xtile;
		xspot[1] = yintercept >> TILESHIFT;
		int32_t xintercept = FixedMul(xstep, ypartial) + viewx;
		short ytile = focalty + ytilestep;
		yspot[0] = xintercept >> TILESHIFT;
		yspot[1] = ytile;
		int texdelta = 0;

		if(playerInPushwallBackTile)
		{
			if(focalspot->pushReceptor)
				focalspot = focalspot->pushReceptor;

			if((focalspot->pushDirection == MapTile::East && xtilestep == 1) ||
				(focalspot->pushDirection == MapTile::West && xtilestep == -1))
			{
				int32_t yintbuf = yintercept -
					ytilestep * (abs(ystep * signed(64 - focalspot->pushAmount)) >> 6);
				if((yintbuf >> TILESHIFT) == focalty)
				{
					if(focalspot->pushDirection == MapTile::East)
						xintercept = (focaltx << TILESHIFT) +
							(focalspot->pushAmount << 10);
					else
						xintercept = (focaltx << TILESHIFT) - TILEGLOBAL +
							((64 - focalspot->pushAmount) << 10);
					yintercept = yintbuf;
					ytile = (short)(yintercept >> TILESHIFT);
					if(fast_trace_wall_column(pixx, xintercept, yintercept,
						focalspot, true, xtilestep, ytilestep, xtile, ytile,
						texdelta, &fast_wall_columns[pixx]) == FastColumnMiss)
					{
						return false;
					}
					continue;
				}
			}
			else if((focalspot->pushDirection == MapTile::South && ytilestep == 1) ||
				(focalspot->pushDirection == MapTile::North && ytilestep == -1))
			{
				int32_t xintbuf = xintercept -
					xtilestep * (abs(xstep * signed(64 - focalspot->pushAmount)) >> 6);
				if((xintbuf >> TILESHIFT) == focaltx)
				{
					xintercept = xintbuf;
					if(focalspot->pushDirection == MapTile::South)
						yintercept = (focalty << TILESHIFT) +
							(focalspot->pushAmount << 10);
					else
						yintercept = (focalty << TILESHIFT) - TILEGLOBAL +
							((64 - focalspot->pushAmount) << 10);
					xtile = (short)(xintercept >> TILESHIFT);
					if(fast_trace_wall_column(pixx, xintercept, yintercept,
						focalspot, false, xtilestep, ytilestep, xtile, ytile,
						texdelta, &fast_wall_columns[pixx]) == FastColumnMiss)
					{
						return false;
					}
					continue;
				}
			}
		}

		do
		{
			if(ytilestep == -1 && (yintercept >> TILESHIFT) <= ytile)
				goto horizentry;
			if(ytilestep == 1 && (yintercept >> TILESHIFT) >= ytile)
				goto horizentry;
vertentry:
			if((uint32_t)yintercept > fast_height * 65536u - 1u ||
				(word)xtile >= fast_width)
			{
				if(xtile < 0)
					xintercept = 0, xtile = 0;
				else if((unsigned)xtile >= fast_width)
					xintercept = fast_width << TILESHIFT,
					xtile = fast_width - 1;
				else
					xtile = (short)(xintercept >> TILESHIFT);
				if(yintercept < 0)
					yintercept = 0, ytile = 0;
				else if((unsigned)yintercept >= (fast_height << TILESHIFT))
					yintercept = fast_height << TILESHIFT,
					ytile = fast_height - 1;
				return false;
				break;
			}
			if(xspot[0] >= fast_width || xspot[1] >= fast_height)
				return false;

			{
				MapSpot spot = fast_spot(xspot[0], xspot[1]);
				if(spot == NULL)
					return false;
				if(spot->tile)
				{
					FastColumnResult result = FastColumnMiss;
					if(spot->tile->offsetVertical)
					{
						MapTile::Side dir = fast_hit_dir(true, xtilestep,
							ytilestep);
						int32_t yintbuf = yintercept + (ystep >> 1);
						if((yintbuf >> 16) != (yintercept >> 16))
							goto passvert;
						if(CheckSlidePass(spot->slideStyle, (word)yintbuf,
							spot->slideAmount[dir]))
						{
							goto passvert;
						}
						yintercept = yintbuf;
						xintercept = (xtile << TILESHIFT) | 0x8000;
						ytile = (short)(yintercept >> TILESHIFT);
						result = fast_trace_wall_column(pixx, xintercept,
							yintercept, spot, true, xtilestep, ytilestep,
							xtile, ytile, texdelta, &fast_wall_columns[pixx]);
					}
					else
					{
						bool isPushwall =
							spot->pushAmount != 0 || spot->pushReceptor;
						if(spot->pushReceptor)
							spot = spot->pushReceptor;

						if(isPushwall)
						{
							if(spot->pushDirection == MapTile::West ||
								spot->pushDirection == MapTile::East)
							{
								int32_t yintbuf;
								int pwallposnorm;
								int pwallposinv;
								if(spot->pushDirection == MapTile::West)
								{
									pwallposnorm = 64 - spot->pushAmount;
									pwallposinv = spot->pushAmount;
								}
								else
								{
									pwallposnorm = spot->pushAmount;
									pwallposinv = 64 - spot->pushAmount;
								}
								if((spot->pushDirection == MapTile::East &&
									xtile == (signed)spot->GetX() &&
									((uint32_t)yintercept >> TILESHIFT) == spot->GetY())
									|| (spot->pushDirection == MapTile::West &&
									!(xtile == (signed)spot->GetX() &&
									((uint32_t)yintercept >> TILESHIFT) == spot->GetY())))
								{
									yintbuf = yintercept + ((ystep * pwallposnorm) >> 6);
									if((yintbuf >> TILESHIFT) !=
										(yintercept >> TILESHIFT))
									{
										goto passvert;
									}

									xintercept = (xtile << TILESHIFT) + TILEGLOBAL -
										(pwallposinv << 10);
									yintercept = yintbuf;
									ytile = (short)(yintercept >> TILESHIFT);
									result = fast_trace_wall_column(pixx,
										xintercept, yintercept, spot, true,
										xtilestep, ytilestep, xtile, ytile,
										texdelta, &fast_wall_columns[pixx]);
								}
								else
								{
									yintbuf = yintercept + ((ystep * pwallposinv) >> 6);
									if((yintbuf >> TILESHIFT) !=
										(yintercept >> TILESHIFT))
									{
										goto passvert;
									}

									xintercept = (xtile << TILESHIFT) -
										(pwallposinv << 10);
									yintercept = yintbuf;
									ytile = (short)(yintercept >> TILESHIFT);
									result = fast_trace_wall_column(pixx,
										xintercept, yintercept, spot, true,
										xtilestep, ytilestep, xtile, ytile,
										texdelta, &fast_wall_columns[pixx]);
								}
							}
							else
							{
								int pwallposi = spot->pushAmount;
								if(spot->pushDirection == MapTile::North)
									pwallposi = 64 - spot->pushAmount;
								if((spot->pushDirection == MapTile::South &&
									(word)yintercept < (pwallposi << 10))
									|| (spot->pushDirection == MapTile::North &&
									(word)yintercept > (pwallposi << 10)))
								{
									if(((uint32_t)yintercept >> TILESHIFT) ==
										spot->GetY() && xtile == (signed)spot->GetX())
									{
										if((spot->pushDirection == MapTile::South &&
											(int32_t)((word)yintercept) + ystep <
											(pwallposi << 10))
											|| (spot->pushDirection == MapTile::North &&
											(int32_t)((word)yintercept) + ystep >
											(pwallposi << 10)))
										{
											goto passvert;
										}

										if(spot->pushDirection == MapTile::South)
										{
											yintercept = (yintercept & 0xffff0000) +
												(pwallposi << 10);
											xintercept = xintercept -
												((xstep * (64 - pwallposi)) >> 6);
										}
										else
										{
											yintercept = (yintercept & 0xffff0000) -
												TILEGLOBAL + (pwallposi << 10);
											xintercept = xintercept -
												((xstep * pwallposi) >> 6);
										}
										xtile = (short)(xintercept >> TILESHIFT);
										result = fast_trace_wall_column(pixx,
											xintercept, yintercept, spot, false,
											xtilestep, ytilestep, xtile, ytile,
											texdelta, &fast_wall_columns[pixx]);
									}
									else
									{
										texdelta = -(pwallposi << 10);
										xintercept = xtile << TILESHIFT;
										ytile = (short)(yintercept >> TILESHIFT);
										result = fast_trace_wall_column(pixx,
											xintercept, yintercept, spot, true,
											xtilestep, ytilestep, xtile, ytile,
											texdelta, &fast_wall_columns[pixx]);
									}
								}
								else
								{
									if(((uint32_t)yintercept >> TILESHIFT) ==
										spot->GetY() && xtile == (signed)spot->GetX())
									{
										texdelta = -(pwallposi << 10);
										xintercept = xtile << TILESHIFT;
										ytile = (short)(yintercept >> TILESHIFT);
										result = fast_trace_wall_column(pixx,
											xintercept, yintercept, spot, true,
											xtilestep, ytilestep, xtile, ytile,
											texdelta, &fast_wall_columns[pixx]);
									}
									else
									{
										if((spot->pushDirection == MapTile::South &&
											(int32_t)((word)yintercept) + ystep >
											(pwallposi << 10))
											|| (spot->pushDirection == MapTile::North &&
											(int32_t)((word)yintercept) + ystep <
											(pwallposi << 10)))
										{
											goto passvert;
										}

										if(spot->pushDirection == MapTile::South)
										{
											yintercept = (yintercept & 0xffff0000) -
												TILEGLOBAL + (pwallposi << 10);
											xintercept = xintercept -
												((xstep * pwallposi) >> 6);
										}
										else
										{
											yintercept = (yintercept & 0xffff0000) +
												(pwallposi << 10);
											xintercept = xintercept -
												((xstep * (64 - pwallposi)) >> 6);
										}
										xtile = (short)(xintercept >> TILESHIFT);
										result = fast_trace_wall_column(pixx,
											xintercept, yintercept, spot, false,
											xtilestep, ytilestep, xtile, ytile,
											texdelta, &fast_wall_columns[pixx]);
									}
								}
							}
						}
						else
						{
							xintercept = xtile << TILESHIFT;
							ytile = (short)(yintercept >> TILESHIFT);
							result = fast_trace_wall_column(pixx,
								xintercept, yintercept, spot, true,
								xtilestep, ytilestep, xtile, ytile,
								texdelta, &fast_wall_columns[pixx]);
						}
					}
					if(result == FastColumnMiss)
						return false;
					break;
				}
passvert:
				spot->visible = true;
				spot->amFlags |= AM_Visible;
				xtile += xtilestep;
				yintercept += ystep;
				xspot[0] = xtile;
				xspot[1] = yintercept >> TILESHIFT;
			}
		}
		while(true);
		continue;

		do
		{
			if(xtilestep == -1 && (xintercept >> TILESHIFT) <= xtile)
				goto vertentry;
			if(xtilestep == 1 && (xintercept >> TILESHIFT) >= xtile)
				goto vertentry;
horizentry:
			if((uint32_t)xintercept > fast_width * 65536u - 1u ||
				(word)ytile >= fast_height)
			{
				if(ytile < 0)
					yintercept = 0, ytile = 0;
				else if((unsigned)ytile >= fast_height)
					yintercept = fast_height << TILESHIFT,
					ytile = fast_height - 1;
				else
					ytile = (short)(yintercept >> TILESHIFT);
				if(xintercept < 0)
					xintercept = 0, xtile = 0;
				else if((unsigned)xintercept >= (fast_width << TILESHIFT))
					xintercept = fast_width << TILESHIFT,
					xtile = fast_width - 1;
				return false;
			}
			if(yspot[0] >= fast_width || yspot[1] >= fast_height)
				return false;

			{
				MapSpot spot = fast_spot(yspot[0], yspot[1]);
				if(spot == NULL)
					return false;
				if(spot->tile)
				{
					FastColumnResult result = FastColumnMiss;
					if(spot->tile->offsetHorizontal)
					{
						MapTile::Side dir = fast_hit_dir(false,
							xtilestep, ytilestep);
						int32_t xintbuf = xintercept + (xstep >> 1);
						if((xintbuf >> TILESHIFT) !=
							(xintercept >> TILESHIFT))
						{
							goto passhoriz;
						}
						if(CheckSlidePass(spot->slideStyle,
							(word)xintbuf, spot->slideAmount[dir]))
						{
							goto passhoriz;
						}
						xintercept = xintbuf;
						yintercept = (ytile << TILESHIFT) | 0x8000;
						xtile = (short)(xintercept >> TILESHIFT);
						result = fast_trace_wall_column(pixx,
							xintercept, yintercept, spot, false,
							xtilestep, ytilestep, xtile, ytile,
							texdelta, &fast_wall_columns[pixx]);
					}
					else
					{
						bool isPushwall =
							spot->pushAmount != 0 || spot->pushReceptor;
						if(spot->pushReceptor)
							spot = spot->pushReceptor;

						if(isPushwall)
						{
							if(spot->pushDirection == MapTile::North ||
								spot->pushDirection == MapTile::South)
							{
								int32_t xintbuf;
								int pwallposnorm;
								int pwallposinv;
								if(spot->pushDirection == MapTile::North)
								{
									pwallposnorm = 64 - spot->pushAmount;
									pwallposinv = spot->pushAmount;
								}
								else
								{
									pwallposnorm = spot->pushAmount;
									pwallposinv = 64 - spot->pushAmount;
								}
								if((spot->pushDirection == MapTile::South &&
									ytile == (signed)spot->GetY() &&
									((uint32_t)xintercept >> TILESHIFT) == spot->GetX())
									|| (spot->pushDirection == MapTile::North &&
									!(ytile == (signed)spot->GetY() &&
									((uint32_t)xintercept >> TILESHIFT) == spot->GetX())))
								{
									xintbuf = xintercept + ((xstep * pwallposnorm) >> 6);
									if((xintbuf >> TILESHIFT) !=
										(xintercept >> TILESHIFT))
									{
										goto passhoriz;
									}

									yintercept = (ytile << TILESHIFT) + TILEGLOBAL -
										(pwallposinv << 10);
									xintercept = xintbuf;
									xtile = (short)(xintercept >> TILESHIFT);
									result = fast_trace_wall_column(pixx,
										xintercept, yintercept, spot, false,
										xtilestep, ytilestep, xtile, ytile,
										texdelta, &fast_wall_columns[pixx]);
								}
								else
								{
									xintbuf = xintercept + ((xstep * pwallposinv) >> 6);
									if((xintbuf >> TILESHIFT) !=
										(xintercept >> TILESHIFT))
									{
										goto passhoriz;
									}

									yintercept = (ytile << TILESHIFT) -
										(pwallposinv << 10);
									xintercept = xintbuf;
									xtile = (short)(xintercept >> TILESHIFT);
									result = fast_trace_wall_column(pixx,
										xintercept, yintercept, spot, false,
										xtilestep, ytilestep, xtile, ytile,
										texdelta, &fast_wall_columns[pixx]);
								}
							}
							else
							{
								int pwallposi = spot->pushAmount;
								if(spot->pushDirection == MapTile::West)
									pwallposi = 64 - spot->pushAmount;
								if((spot->pushDirection == MapTile::East &&
									(word)xintercept < (pwallposi << 10))
									|| (spot->pushDirection == MapTile::West &&
									(word)xintercept > (pwallposi << 10)))
								{
									if(((uint32_t)xintercept >> TILESHIFT) ==
										spot->GetX() && ytile == (signed)spot->GetY())
									{
										if((spot->pushDirection == MapTile::East &&
											(int32_t)((word)xintercept) + xstep <
											(pwallposi << 10))
											|| (spot->pushDirection == MapTile::West &&
											(int32_t)((word)xintercept) + xstep >
											(pwallposi << 10)))
										{
											goto passhoriz;
										}

										if(spot->pushDirection == MapTile::East)
										{
											xintercept = (xintercept & 0xffff0000) +
												(pwallposi << 10);
											yintercept = yintercept -
												((ystep * (64 - pwallposi)) >> 6);
										}
										else
										{
											xintercept = (xintercept & 0xffff0000) -
												TILEGLOBAL + (pwallposi << 10);
											yintercept = yintercept -
												((ystep * pwallposi) >> 6);
										}
										ytile = (short)(yintercept >> TILESHIFT);
										result = fast_trace_wall_column(pixx,
											xintercept, yintercept, spot, true,
											xtilestep, ytilestep, xtile, ytile,
											texdelta, &fast_wall_columns[pixx]);
									}
									else
									{
										texdelta = -(pwallposi << 10);
										yintercept = ytile << TILESHIFT;
										xtile = (short)(xintercept >> TILESHIFT);
										result = fast_trace_wall_column(pixx,
											xintercept, yintercept, spot, false,
											xtilestep, ytilestep, xtile, ytile,
											texdelta, &fast_wall_columns[pixx]);
									}
								}
								else
								{
									if(((uint32_t)xintercept >> TILESHIFT) ==
										spot->GetX() && ytile == (signed)spot->GetY())
									{
										texdelta = -(pwallposi << 10);
										yintercept = ytile << TILESHIFT;
										xtile = (short)(xintercept >> TILESHIFT);
										result = fast_trace_wall_column(pixx,
											xintercept, yintercept, spot, false,
											xtilestep, ytilestep, xtile, ytile,
											texdelta, &fast_wall_columns[pixx]);
									}
									else
									{
										if((spot->pushDirection == MapTile::East &&
											(int32_t)((word)xintercept) + xstep >
											(pwallposi << 10))
											|| (spot->pushDirection == MapTile::West &&
											(int32_t)((word)xintercept) + xstep <
											(pwallposi << 10)))
										{
											goto passhoriz;
										}

										if(spot->pushDirection == MapTile::East)
										{
											xintercept = (xintercept & 0xffff0000) -
												TILEGLOBAL + (pwallposi << 10);
											yintercept = yintercept -
												((ystep * pwallposi) >> 6);
										}
										else
										{
											xintercept = (xintercept & 0xffff0000) +
												(pwallposi << 10);
											yintercept = yintercept -
												((ystep * (64 - pwallposi)) >> 6);
										}
										ytile = (short)(yintercept >> TILESHIFT);
										result = fast_trace_wall_column(pixx,
											xintercept, yintercept, spot, true,
											xtilestep, ytilestep, xtile, ytile,
											texdelta, &fast_wall_columns[pixx]);
									}
								}
							}
						}
						else
						{
							yintercept = ytile << TILESHIFT;
							xtile = (short)(xintercept >> TILESHIFT);
							result = fast_trace_wall_column(pixx,
								xintercept, yintercept, spot, false,
								xtilestep, ytilestep, xtile, ytile,
								texdelta, &fast_wall_columns[pixx]);
						}
					}
					if(result == FastColumnMiss)
						return false;
					goto next_column;
				}
passhoriz:
				spot->visible = true;
				spot->amFlags |= AM_Visible;
				ytile += ytilestep;
				xintercept += xstep;
				yspot[0] = xintercept >> TILESHIFT;
				yspot[1] = ytile;
			}
		}
		while(true);
next_column:
		;
	}

	return true;
}

static void fast_submit_columns(byte *framebuffer)
{
	for(int pixx = 0; pixx < viewwidth; ++pixx)
	{
		if(fast_wall_columns[pixx].count > 0)
			fast_submit_wall_column(framebuffer, fast_wall_columns[pixx]);
	}
}

bool OF_WolfFastRenderer_RenderWalls(byte *framebuffer, unsigned pitch)
{
	if(framebuffer == NULL || pitch == 0)
		return false;
	if(viewwidth <= 0 || viewwidth > FAST_MAX_COLUMNS)
		return false;
	if(!fast_ensure_map_cache())
		return false;
	fast_pitch = pitch;

	fast_texture_frame++;
	if(fast_texture_frame == 0)
		fast_texture_frame = 1;

	focaltx = (short)(viewx >> TILESHIFT);
	focalty = (short)(viewy >> TILESHIFT);
	viewtx = (short)(players[ConsolePlayer].camera->x >> TILESHIFT);
	viewty = (short)(players[ConsolePlayer].camera->y >> TILESHIFT);

	min_wallheight = viewheight;
	viewshift = FixedMul(focallengthy,
		finetangent[(ANGLE_180 + players[ConsolePlayer].camera->pitch) >>
			ANGLETOFINESHIFT]);

	const angle_t bobangle =
		((gamestate.TimeCount << 13) / (20 * TICRATE / 35)) & FINEMASK;
	const fixed playerMovebob =
		players[ConsolePlayer].mo->GetClass()->Meta.GetMetaFixed(APMETA_MoveBob);
	const fixed curbob = gamestate.victoryflag ? 0 :
		FixedMul(FixedMul(players[ConsolePlayer].bob, playerMovebob) >> 1,
			finesine[bobangle]);
	viewz = curbob - players[ConsolePlayer].mo->viewheight;

	if(!fast_trace_columns())
		return false;

	fast_submit_columns(framebuffer);
	return true;
}

#endif
