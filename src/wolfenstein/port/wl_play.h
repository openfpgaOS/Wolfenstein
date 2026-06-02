#ifndef __WL_PLAY_H__
#define __WL_PLAY_H__

/*
=============================================================================

							WL_PLAY DEFINITIONS

=============================================================================
*/

#define BASEMOVE                35
#define RUNMOVE                 70
#define BASETURN                35
#define RUNTURN                 70

#define JOYSCALE                2

extern  bool noadaptive;
extern  unsigned        tics;
extern  fixed           renderfraction;
extern  int32_t         renderbasetimecount;
extern  int             viewsize;
extern unsigned short Paused;

static inline fixed R_InterpolateFixed(fixed oldvalue, fixed value)
{
	return oldvalue + FixedMul(value - oldvalue, renderfraction);
}

static inline angle_t R_InterpolateAngle(angle_t oldvalue, angle_t value)
{
	const int32_t diff = (int32_t)(value - oldvalue);
	return oldvalue + (angle_t)(((int64_t)diff * renderfraction) >> FRACBITS);
}

static inline int64_t R_InterpolatedTimeScaled(int shift)
{
	return ((int64_t)renderbasetimecount << shift) +
		(((int64_t)renderfraction << shift) >> FRACBITS);
}

static inline int64_t R_InterpolatedTimeMul(int multiplier)
{
	return (int64_t)renderbasetimecount * multiplier +
		(((int64_t)renderfraction * multiplier) >> FRACBITS);
}

static inline real64 R_InterpolatedTimeCount()
{
	return (real64)renderbasetimecount + (real64)renderfraction / (real64)FRACUNIT;
}

//
// current user input
//
struct TicCmd_t
{
	int controlx,controly, controlstrafe; // range from -100 to 100
	int controlpanx, controlpany;
	BYTE buttonstate[NUMBUTTONS], ambuttonstate[NUMAMBUTTONS];
	BYTE buttonheld[NUMBUTTONS], ambuttonheld[NUMAMBUTTONS];
};
extern unsigned int ConsolePlayer;
extern TicCmd_t control[MAXPLAYERS];
extern  exit_t      playstate;
extern  bool        madenoise;
extern  int         godmode;
extern	bool		notargetmode;

extern  bool        demorecord,demoplayback;
extern  int8_t      *demoptr, *lastdemoptr;
extern  memptr      demobuffer;

void    PlayFrame();
void    PlayLoop (void);

void    InitRedShifts (void);
void    FinishPaletteShifts (void);

void    CheckKeys();
void    PollControls (bool);
int     StopMusic(void);
void    StartMusic(void);
void    ContinueMusic(int offs);
void    StartDamageFlash (int damage);
void    StartBonusFlash (void);

void CalcTics();
void Delay(int wolfticks);
int32_t GetTimeCount();
void ResetTimeCount();

extern  int32_t     funnyticount;           // FOR FUNNY BJ FACE

extern  bool        noclip,ammocheat,mouselook;
extern  int         singlestep;
extern  unsigned int extravbls;

#endif
