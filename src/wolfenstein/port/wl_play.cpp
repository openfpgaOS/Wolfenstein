// WL_PLAY.C

#include "c_cvars.h"
#include "wl_def.h"
#include "wl_menu.h"
#include "id_ca.h"
#include "id_sd.h"
#include "id_vl.h"
#include "id_vh.h"
#include "id_us.h"

#include "wl_cloudsky.h"
#include "wl_shade.h"
#include "language.h"
#include "lumpremap.h"
#include "thinker.h"
#include "actor.h"
#include "textures/textures.h"
#include "v_video.h"
#include "wl_agent.h"
#include "wl_debug.h"
#include "wl_draw.h"
#include "wl_game.h"
#include "wl_inter.h"
#include "wl_net.h"
#include "wl_play.h"
#include "g_mapinfo.h"
#include "a_inventory.h"
#include "am_map.h"
#include "of_ecwolf_gpu.h"

/*
=============================================================================

												LOCAL CONSTANTS

=============================================================================
*/

#define sc_Question     0x35

/*
=============================================================================

												GLOBAL VARIABLES

=============================================================================
*/

bool madenoise;              // true when shooting or screaming

exit_t playstate;

#ifdef __ANDROID__
extern bool ShadowingEnabled;
#endif

bool noclip, ammocheat, mouselook = false;
int godmode, singlestep;
bool notargetmode = false;
unsigned int extravbls = 0; // to remove flicker (gray stuff at the bottom)
unsigned short Paused;

//
// replacing refresh manager
//
bool noadaptive = false;
unsigned tics;
fixed renderfraction = FRACUNIT;
int32_t renderbasetimecount = 0;

//
// control info
//
#define JoyAx(x) (32+(x<<1))
#define CS_AxisDigital -1
ControlScheme controlScheme[] =
{
	{ bt_moveforward,		"Forward",		JoyAx(1),	sc_UpArrow,		-1, offsetof(TicCmd_t, controly), 1 },
	{ bt_movebackward,		"Backward",		JoyAx(1)+1,	sc_DownArrow,	-1, offsetof(TicCmd_t, controly), 0 },
	{ bt_strafeleft,		"Strafe Left",	JoyAx(0),	sc_Comma,		-1, offsetof(TicCmd_t, controlstrafe), 1 },
	{ bt_straferight,		"Strafe Right",	JoyAx(0)+1,	sc_Peroid,		-1, offsetof(TicCmd_t, controlstrafe), 0 },
	{ bt_turnleft,			"Turn Left",	JoyAx(3),	sc_LeftArrow,	-1, offsetof(TicCmd_t, controlx), 1 },
	{ bt_turnright,			"Turn Right",	JoyAx(3)+1,	sc_RightArrow,	-1, offsetof(TicCmd_t, controlx), 0 },
	{ bt_attack,			"Attack",		0,			sc_Control,		0,  CS_AxisDigital, 0},
	{ bt_strafe,			"Strafe",		3,			sc_Alt,			-1, CS_AxisDigital, 0 },
	{ bt_run,				"Run",			2,			sc_LShift,		-1, CS_AxisDigital, 0 },
	{ bt_use,				"Use",			1,			sc_Space,		-1, CS_AxisDigital, 0 },
	{ bt_slot1,				"Slot 1",		-1,			sc_1,			-1, CS_AxisDigital, 0 },
	{ bt_slot2,				"Slot 2", 		-1,			sc_2,			-1, CS_AxisDigital, 0 },
	{ bt_slot3,				"Slot 3",		-1,			sc_3,			-1, CS_AxisDigital, 0 },
	{ bt_slot4,				"Slot 4",		-1,			sc_4,			-1, CS_AxisDigital, 0 },
	{ bt_slot5,				"Slot 5",		-1,			sc_5,			-1, CS_AxisDigital, 0 },
	{ bt_slot6,				"Slot 6",		-1,			sc_6,			-1, CS_AxisDigital, 0 },
	{ bt_slot7,				"Slot 7",		-1,			sc_7,			-1, CS_AxisDigital, 0 },
	{ bt_slot8,				"Slot 8",		-1,			sc_8,			-1, CS_AxisDigital, 0 },
	{ bt_slot9,				"Slot 9",		-1,			sc_9,			-1, CS_AxisDigital, 0 },
	{ bt_slot0,				"Slot 0",		-1,			sc_0,			-1, CS_AxisDigital, 0 },
	{ bt_nextweapon,		"Next Weapon",	4,			-1,				-1, CS_AxisDigital, 0 },
	{ bt_prevweapon,		"Prev Weapon",	5, 			-1,				-1, CS_AxisDigital, 0 },
	{ bt_altattack,			"Alt Attack",	-1,			-1,				-1, CS_AxisDigital, 0 },
	{ bt_reload,			"Reload",		-1,			-1,				-1, CS_AxisDigital, 0 },
	{ bt_zoom,				"Zoom",			-1,			-1,				-1, CS_AxisDigital, 0 },
	{ bt_automap,			"Automap",		-1,			-1,				-1, CS_AxisDigital, 0 },
	{ bt_showstatusbar,		"Show Status",	-1,			sc_Tab,			-1,	CS_AxisDigital, 0 },
	{ bt_pause,				"Pause",		-1,			sc_Pause,		-1, CS_AxisDigital, 0 },
	{ bt_esc,				"Main Menu",	-1,			-1,				-1, CS_AxisDigital, 0 },

	// End of List
	{ bt_nobutton,			NULL, -1, -1, -1, CS_AxisDigital, 0 }
};
ControlScheme &schemeAutomapKey = controlScheme[25]; // When the input system is redone, hopefully we don't need this kind of thing

ControlScheme amControlScheme[] =
{
	{ bt_zoomin,			"Zoom In",		JoyAx(2),	sc_Equals,		-1, -1, 0 },
	{ bt_zoomout,			"Zoom Out",		JoyAx(2)+1,	sc_Minus,		-1, -1, 0 },
	{ bt_panup,				"Pan Up",		JoyAx(1),	sc_UpArrow,		-1, offsetof(TicCmd_t, controlpany), 0 },
	{ bt_pandown,			"Pan Down",		JoyAx(1)+1,	sc_DownArrow,	-1, offsetof(TicCmd_t, controlpany), 1 },
	{ bt_panleft,			"Pan Left",		JoyAx(0),	sc_LeftArrow,	-1, offsetof(TicCmd_t, controlpanx), 0 },
	{ bt_panright,			"Pan Right",	JoyAx(0)+1,	sc_RightArrow,	-1, offsetof(TicCmd_t, controlpanx), 1 },

	{ bt_nobutton,			NULL, -1, -1, -1, -1, 0 }
};

void ControlScheme::setKeyboard(ControlScheme* scheme, Button button, int value)
{
	for(int i = 0;scheme[i].button != bt_nobutton;i++)
	{
		if(scheme[i].keyboard == value)
			scheme[i].keyboard = -1;
		if(scheme[i].button == button)
			scheme[i].keyboard = value;
	}
}

void ControlScheme::setJoystick(ControlScheme* scheme, Button button, int value)
{
	for(int i = 0;scheme[i].button != bt_nobutton;i++)
	{
		if(scheme[i].joystick == value)
			scheme[i].joystick = -1;
		if(scheme[i].button == button)
			scheme[i].joystick = value;
	}
}

void ControlScheme::setMouse(ControlScheme* scheme, Button button, int value)
{
	for(int i = 0;scheme[i].button != bt_nobutton;i++)
	{
		if(scheme[i].mouse == value)
			scheme[i].mouse = -1;
		if(scheme[i].button == button)
			scheme[i].mouse = value;
	}
}

int viewsize;

bool demorecord, demoplayback;
int8_t *demoptr, *lastdemoptr;
memptr demobuffer;

//
// current user input
//
unsigned int ConsolePlayer = 0;
TicCmd_t control[MAXPLAYERS];

//===========================================================================


void CenterWindow (word w, word h);
int StopMusic (void);
void StartMusic (void);
void ContinueMusic (int offs);
void PlayLoop (void);

/*
=============================================================================

							TIMING

=============================================================================
*/

static int32_t lasttimecount;

static uint64_t GetTimeUS()
{
	const uint64_t counter = SDL_GetPerformanceCounter();
	const uint64_t frequency = SDL_GetPerformanceFrequency();
	if(frequency == 0)
		return (uint64_t)SDL_GetTicks() * 1000ull;
	if(frequency == 1000000ull)
		return counter;
	return (counter / frequency) * 1000000ull +
		(counter % frequency) * 1000000ull / frequency;
}

static uint64_t TicsToUS(uint32_t timecount)
{
	return (uint64_t)timecount * 1000000ull / TICRATE;
}

int32_t GetTimeCount()
{
	return (int32_t)(GetTimeUS() * TICRATE / 1000000ull);
}

static void UseCurrentRenderTime()
{
	renderfraction = FRACUNIT;
	renderbasetimecount = gamestate.TimeCount;
}

static void UpdateRenderInterpolation()
{
	if((Paused & 1) || Net::IsBlocked() || gamestate.TimeCount <= 0)
	{
		UseCurrentRenderTime();
		return;
	}

	/* lasttimecount is the absolute wall-clock tic index of the newest
	 * simulated tic (CalcTics keeps it synced to GetTimeCount()), so the
	 * newest state becomes fully current exactly one tic period after
	 * TicsToUS(lasttimecount).  Anchoring on that exact boundary -- rather
	 * than re-deriving it from the phase of "now" -- keeps the time mapping
	 * consistent when the present path blocks on the display and the render
	 * slides past a period boundary. */
	uint64_t curtime = GetTimeUS();

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
	/* When the loop is display-locked (the last buffer acquire blocked until
	 * the previous swap was consumed at vsync), re-anchor the time sample on
	 * that vsync instead of "now".  The wall-clock distance from vsync to
	 * this call varies with maintenance/sim load every frame, and with a
	 * fixed display cadence that variance otherwise shows up directly as
	 * motion jitter.  Sampling vsync+4ms makes the rendered timeline a pure
	 * function of the display clock; the 4 ms skew keeps the fraction inside
	 * the interpolation window for any tic phase. */
	uint32_t sinceSync;
	if(OF_WolfGPU_USSinceDisplaySync(&sinceSync) && sinceSync < 20000u)
		curtime = curtime - sinceSync + 4000u;
#endif

	const uint64_t anchor = TicsToUS((uint32_t)lasttimecount);
	uint64_t frac = 0;
	if(curtime > anchor)
		frac = (curtime - anchor) * TICRATE * FRACUNIT / 1000000ull;
	/* Allow bounded extrapolation past the newest tic instead of clamping:
	 * the sample point can legitimately sit a few ms beyond the newest
	 * snapshot (a tic boundary crossed between CalcTics and here).  Clamping
	 * held the previous state for a frame -- a visible stutter while turning
	 * at a constant rate; extrapolating the last tic's delta renders
	 * constant-rate motion exactly and costs at most a third of a tic of
	 * overshoot for one frame when motion stops. */
	const uint64_t fracmax = FRACUNIT + FRACUNIT / 3;
	if(frac > fracmax)
		frac = fracmax;
	renderfraction = (fixed)frac;
	renderbasetimecount = gamestate.TimeCount - 1;
}

static void SnapshotPlayerRenderStates()
{
	for(unsigned int i = 0;i < MAXPLAYERS;++i)
		players[i].oldbob = players[i].bob;
}

static void SyncPlayerRenderStates()
{
	SnapshotPlayerRenderStates();
}

static bool AnyPlayerNeedsSpawn()
{
	for(unsigned int p = 0;p < Net::InitVars.numPlayers;++p)
	{
		if(players[p].state == player_t::PST_ENTER ||
			players[p].state == player_t::PST_REBORN)
		{
			return true;
		}
	}
	return false;
}

/*
=====================
=
= CalcTics
=
=====================
*/

void CalcTics()
{
	int32_t curtimecount = GetTimeCount();

//
// calculate tics since last refresh for adaptive timing
//

	// Have we arrived too soon?
	while(lasttimecount == curtimecount + 1)
	{
		SDL_Delay(1);
		curtimecount = GetTimeCount();
	}

	// Detect rollover, particularly if the game were paused for a LONG time
	if(lasttimecount > curtimecount)
	{
		ResetTimeCount();
		curtimecount = lasttimecount;
	}

	uint64_t curtimeus = GetTimeUS();
	curtimecount = (int32_t)(curtimeus * TICRATE / 1000000ull);
	tics = curtimecount - lasttimecount;
	if(!tics)
	{
		// wait until end of current tic
		const uint64_t targetus = TicsToUS((uint32_t)(lasttimecount + 1));
		while(targetus > curtimeus)
		{
			const uint64_t waitus = targetus - curtimeus;
			if(waitus > 1500ull)
				SDL_Delay((Uint32)((waitus - 500ull) / 1000ull));
			curtimeus = GetTimeUS();
		}
		tics = 1;
	}
	else if(Net::IsBlocked())
		tics = 1;
#if !defined(OF_ECWOLF_OPENFPGA) || defined(OF_PC)
	else if(noadaptive)
		tics = 1;
#else
	// OpenFPGA presents at display cadence; capping to one 70 Hz tic per
	// 60 Hz frame slows gameplay, so keep real elapsed tic catch-up.
	(void)noadaptive;
#endif

	lasttimecount += tics;

	if (tics>MAXTICS)
		tics = MAXTICS;
}

void ResetTimeCount()
{
	lasttimecount = GetTimeCount();
	UseCurrentRenderTime();
}

void Delay(int wolfticks)
{
	if(wolfticks>0)
		SDL_Delay(TICS2MS(wolfticks));
}

/*
=============================================================================

							USER CONTROL

=============================================================================
*/

/*
===================
=
= PollKeyboardButtons
=
===================
*/

void PollKeyboardButtons (void)
{
	if(automap == AMA_Normal)
	{
		// HACK
		bool jam[512] = {false};
		bool jamall = !!(Paused & 2); // Paused for automap

		for(int i = 0;jamall ? amControlScheme[i].button != bt_nobutton : amControlScheme[i].button <= bt_zoomout;i++)
		{
			if(amControlScheme[i].keyboard != -1 && Keyboard[amControlScheme[i].keyboard])
			{
				control[ConsolePlayer].ambuttonstate[amControlScheme[i].button] = true;
				jam[amControlScheme[i].keyboard] = true;
			}
		}
		for(int i = 0;controlScheme[i].button != bt_nobutton;i++)
		{
			if(controlScheme[i].keyboard != -1 && Keyboard[controlScheme[i].keyboard] && !jam[controlScheme[i].keyboard])
				control[ConsolePlayer].buttonstate[controlScheme[i].button] = true;
		}
	}
	else
	{
		for(int i = 0;controlScheme[i].button != bt_nobutton;i++)
		{
			if(controlScheme[i].keyboard != -1 && Keyboard[controlScheme[i].keyboard])
				control[ConsolePlayer].buttonstate[controlScheme[i].button] = true;
		}
	}

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
	// The OpenFPGA SDL shim delivers the L2/R2 triggers as PageDown/PageUp
	// key state (they have no SDL controller-button equivalent), so bind
	// them here instead of taking over the scheme table's single keyboard
	// slot per action: L2 opens doors, R2 fires.  The face buttons keep
	// their regular bindings.
	if(Keyboard[sc_PgDn])
		control[ConsolePlayer].buttonstate[bt_use] = true;
	if(Keyboard[sc_PgUp])
		control[ConsolePlayer].buttonstate[bt_attack] = true;
#endif
}


/*
===================
=
= PollMouseButtons
=
===================
*/

void PollMouseButtons (void)
{
	int buttons = IN_MouseButtons();
	for (int i = 0; controlScheme[i].button != bt_nobutton; i++)
	{
		if (controlScheme[i].mouse == -1)
			continue;

		BYTE &state = control[ConsolePlayer].buttonstate[controlScheme[i].button];
		switch(controlScheme[i].mouse)
		{
		case ControlScheme::MWheel_Left:
			if (MouseWheel[di_west])
				state = true;
			break;
		case ControlScheme::MWheel_Right:
			if (MouseWheel[di_east])
				state = true;
			break;
		case ControlScheme::MWheel_Down:
			if (MouseWheel[di_south])
				state = true;
			break;
		case ControlScheme::MWheel_Up:
			if (MouseWheel[di_north])
				state = true;
			break;
		default:
			if ((buttons & (1 << controlScheme[i].mouse)))
				state = true;
			break;
		}
	}

	IN_ClearWheel();
}



/*
===================
=
= PollJoystickButtons
=
===================
*/

void PollJoystickButtons (void)
{
	if(automap == AMA_Normal)
	{
		// HACK
		bool jam[64] = {false};
		bool jamall = !!(Paused & 2); // Paused for automap

		int buttons = IN_JoyButtons();
		int axes = IN_JoyAxes();
		for(int i = 0;jamall ? amControlScheme[i].button != bt_nobutton : amControlScheme[i].button <= bt_zoomout;i++)
		{
			if(amControlScheme[i].joystick != -1)
			{
				if(amControlScheme[i].joystick < 32 && (buttons & (1<<amControlScheme[i].joystick)))
				{
					control[ConsolePlayer].ambuttonstate[amControlScheme[i].button] = true;
					jam[amControlScheme[i].joystick] = true;
				}
				else if(amControlScheme[i].axis == -1 && amControlScheme[i].joystick >= 32 && (axes & (1<<(amControlScheme[i].joystick-32))))
				{
					control[ConsolePlayer].ambuttonstate[amControlScheme[i].button] = true;
					jam[amControlScheme[i].joystick] = true;
				}
			}
		}
		for(int i = 0;controlScheme[i].button != bt_nobutton;i++)
		{
			if(controlScheme[i].joystick != -1 && !jam[controlScheme[i].joystick])
			{
				if(controlScheme[i].joystick < 32 && (buttons & (1<<controlScheme[i].joystick)))
					control[ConsolePlayer].buttonstate[controlScheme[i].button] = true;
				else if(controlScheme[i].axis == -1 && controlScheme[i].joystick >= 32 && (axes & (1<<(controlScheme[i].joystick-32))))
					control[ConsolePlayer].buttonstate[controlScheme[i].button] = true;
			}
		}
	}
	else
	{
		int buttons = IN_JoyButtons();
		int axes = IN_JoyAxes();
		for(int i = 0;controlScheme[i].button != bt_nobutton;i++)
		{
			if(controlScheme[i].joystick != -1)
			{
				if(controlScheme[i].joystick < 32 && (buttons & (1<<controlScheme[i].joystick)))
					control[ConsolePlayer].buttonstate[controlScheme[i].button] = true;
				else if(controlScheme[i].axis == -1 && controlScheme[i].joystick >= 32 && (axes & (1<<(controlScheme[i].joystick-32))))
					control[ConsolePlayer].buttonstate[controlScheme[i].button] = true;
			}
		}
	}
}


/*
===================
=
= PollKeyboardMove
=
===================
*/

void PollKeyboardMove (void)
{
	TicCmd_t &cmd = control[ConsolePlayer];

	int delta = (!alwaysrun && cmd.buttonstate[bt_run]) || (alwaysrun && !cmd.buttonstate[bt_run]) ? RUNMOVE : BASEMOVE;

	if(cmd.buttonstate[bt_moveforward])
		cmd.controly -= delta;
	if(cmd.buttonstate[bt_movebackward])
		cmd.controly += delta;
	if(cmd.buttonstate[bt_turnleft])
		cmd.controlx -= delta;
	if(cmd.buttonstate[bt_turnright])
		cmd.controlx += delta;
	if(cmd.buttonstate[bt_strafeleft])
		cmd.controlstrafe -= delta;
	if(cmd.buttonstate[bt_straferight])
		cmd.controlstrafe += delta;
}


/*
===================
=
= PollMouseMove
=
===================
*/

void PollMouseMove (void)
{
	SDL_GetRelativeMouseState(&control[ConsolePlayer].controlpanx, &control[ConsolePlayer].controlpany);

	control[ConsolePlayer].controlx += control[ConsolePlayer].controlpanx * 20 / (21 - mousexadjustment);
	if(mouselook)
	{
		int mousey = control[ConsolePlayer].controlpany;

		if(players[ConsolePlayer].ReadyWeapon && players[ConsolePlayer].ReadyWeapon->fovscale > 0)
			mousey = xs_ToInt(control[ConsolePlayer].controlpany*fabsf(players[ConsolePlayer].ReadyWeapon->fovscale));

		players[ConsolePlayer].mo->pitch += mousey * (ANGLE_1 / (21 - mouseyadjustment));
		if(players[ConsolePlayer].mo->pitch+ANGLE_180 > ANGLE_180+56*ANGLE_1)
			players[ConsolePlayer].mo->pitch = 56*ANGLE_1;
		else if(players[ConsolePlayer].mo->pitch+ANGLE_180 < ANGLE_180-56*ANGLE_1)
			players[ConsolePlayer].mo->pitch = ANGLE_NEG(56*ANGLE_1);
	}
	else if(!mouseyaxisdisabled)
		control[ConsolePlayer].controly += control[ConsolePlayer].controlpany * 40 / (21 - mouseyadjustment);
}


/*
===================
=
= PollJoystickMove
=
===================
*/

void PollJoystickMove (void)
{
	const bool useam = automap == AMA_Normal && Paused;
	const ControlScheme *scheme = useam ? amControlScheme+2 : controlScheme;
	do
	{
		if(scheme->joystick >= 32)
		{
			int axisnum = (scheme->joystick-32)>>1;
			bool positive = (scheme->joystick&1) != 0;
			// Match the digital controls: a fully deflected stick at the
			// default sensitivity (10) contributes exactly BASEMOVE/BASETURN
			// per tic, doubled by the run shift below -- the same rates as a
			// held key or d-pad.  (The old scale topped out at 5*sensitivity
			// = 50 at default, making analog sticks ~43% faster than digital
			// input.)  The response is quadratic: full deflection keeps the
			// digital speed, but small and mid deflections are softened for
			// finer aiming -- a linear stick felt touchy around the center.
			const int rawaxis = clamp(IN_GetJoyAxis(axisnum), -0x7FFF, 0x7FFF);
			const int dzfactor = clamp(JoySensitivity[axisnum].deadzone*0x8000/20, 0, 0x7FFF);
			const int range = 0x8000 - dzfactor;
			const int norm = clamp(abs(rawaxis)+1-dzfactor, 0, 0x8000)*256/range; // 0..256
			int axis = norm*norm*BASEMOVE*JoySensitivity[axisnum].sensitivity/(10*256*256);
			if(useam)
				axis >>= 2;
			else if(control[ConsolePlayer].buttonstate[bt_run])
				axis <<= 1;
			if(positive ^ (rawaxis < 0))
				*(int*)((char*)&control[ConsolePlayer] + scheme->axis) += scheme->negative ? -axis : axis;
		}
	}
	while((++scheme)->axis != CS_AxisDigital);
}

/*
===================
=
= PollControls
=
= Gets user or demo input
= Enable absolute positioning once per frame. This prevents absolute devices
= from being carried over to adaptive tics.
=
= controlx              set between -100 and 100 per tic
= controly
= buttonheld[]  the state of the buttons LAST frame
= buttonstate[] the state of the buttons THIS frame
=
===================
*/

void PollControls (bool absolutes)
{
	int i;
	byte buttonbits;

	TicCmd_t &cmd = control[ConsolePlayer];

	cmd.controlx = 0;
	cmd.controly = 0;
	cmd.controlpanx = 0;
	cmd.controlpany = 0;
	cmd.controlstrafe = 0;
	memcpy (cmd.buttonheld, cmd.buttonstate, sizeof (cmd.buttonstate));
	memset (cmd.buttonstate, 0, sizeof (cmd.buttonstate));
	if (automap)
	{
		memcpy (cmd.ambuttonheld, cmd.ambuttonstate, sizeof (cmd.ambuttonstate));
		memset (cmd.ambuttonstate, 0, sizeof (cmd.ambuttonstate));
	}

	if (demoplayback)
	{
		//
		// read commands from demo buffer
		//
		buttonbits = *demoptr++;
		for (i = 0; i < NUMBUTTONS; i++)
		{
			cmd.buttonstate[i] = buttonbits & 1;
			buttonbits >>= 1;
		}

		cmd.controlx = *demoptr++;
		cmd.controly = *demoptr++;

		if (demoptr == lastdemoptr)
			playstate = ex_completed;   // demo is done

		return;
	}


//
// get button states
//
	PollKeyboardButtons ();

	if (mouseenabled && IN_IsInputGrabbed())
		PollMouseButtons ();

	if (joystickenabled && IN_JoyPresent())
		PollJoystickButtons ();

//
// get movements
//
	PollKeyboardMove ();

	if (absolutes && mouseenabled && IN_IsInputGrabbed())
		PollMouseMove ();

	if (joystickenabled && IN_JoyPresent())
		PollJoystickMove ();

#ifdef __ANDROID__
	extern void pollAndroidControls();
	pollAndroidControls();
#endif

	if (demorecord)
	{
		//
		// save info out to demo buffer
		//
		buttonbits = 0;

		// TODO: Support 32-bit buttonbits
		for (i = NUMBUTTONS - 1; i >= 0; i--)
		{
			buttonbits <<= 1;
			if (cmd.buttonstate[i])
				buttonbits |= 1;
		}

		*demoptr++ = buttonbits;
		*demoptr++ = cmd.controlx;
		*demoptr++ = cmd.controly;

		if (demoptr >= lastdemoptr - 8)
			playstate = ex_completed;
	}
	else if(Net::InitVars.mode != Net::MODE_SinglePlayer)
		Net::PollControls();

	// Check automap toggle before we set any buttons as held
	if (cmd.buttonstate[bt_automap] && !cmd.buttonheld[bt_automap])
	{
		AM_Toggle();
	}
	if (automap)
	{
		AM_CheckKeys();
	}

	for(unsigned int i = 0;i < Net::InitVars.numPlayers;++i)
	{
		if(control[i].buttonstate[bt_pause] && !control[i].buttonheld[bt_pause])
		{
			Paused ^= 1;

			static int lastoffs;
			if(Paused & 1)
			{
				lastoffs = StopMusic();
				IN_ReleaseMouse();
			}
			else
			{
				IN_GrabMouse();
				ContinueMusic(lastoffs);
				if (MousePresent && IN_IsInputGrabbed())
					IN_CenterMouse();     // Clear accumulated mouse movement
				ResetTimeCount();
			}
		}
	}
}

// This should be called once per frame
void ProcessEvents()
{
	IN_ProcessEvents();

//
// get timing info for last frame
//
	if (demoplayback || demorecord)   // demo recording and playback needs to be constant
	{
		// wait up to DEMOTICS Wolf tics
		uint32_t curtime = SDL_GetTicks();
		lasttimecount += DEMOTICS;
		int32_t timediff = TICS2MS(lasttimecount) - curtime;
		if(timediff > 0)
			SDL_Delay(timediff);

		if(timediff < -2 * DEMOTICS)       // more than 2-times DEMOTICS behind?
			lasttimecount = GetTimeCount();      // yes, set to current timecount

		tics = DEMOTICS;
	}
	else
		CalcTics ();
}

//===========================================================================


void BumpGamma()
{
	screenGamma += 0.1f;
	if(screenGamma > 3.0f)
		screenGamma = 1.0f;
	screen->SetGamma(screenGamma);
	US_CenterWindow (10,2);
	FString msg;
	msg.Format("Gamma: %g", screenGamma);
	US_PrintCentered (msg);
	VW_UpdateScreen();
	IN_Ack(ACK_Block);
}

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
static void OF_WolfReturnToGameFrame()
{
	if(startgame || loadedgame)
		return;

	if(viewsize != 21)
	{
		VH_AcquireDeferredScreenLock();
		DrawPlayScreen();
	}
	else
		OF_WolfGPU_SetNextVideoFramePreserve(false);

	PlayFrame();
}
#endif

/*
=====================
=
= CheckKeys
=
= This should only cover control panel keys, debug mode key checks have been
= moved to CheckDebugKeys.
=
=====================
*/

void CheckKeys (void)
{
	static bool changeSize = true;
	ScanCode scan;


	if (screenfaded || demoplayback)    // don't do anything with a faded screen
		return;

	scan = LastScan;

	// [BL] Allow changing the screen size with the -/= keys a la Doom.
	if(automap != AMA_Normal && changeSize)
	{
		if(Keyboard[sc_Equals] && !Keyboard[sc_Minus])
			NewViewSize(viewsize+1);
		else if(!Keyboard[sc_Equals] && Keyboard[sc_Minus])
			NewViewSize(viewsize-1);
		if(Keyboard[sc_Equals] || Keyboard[sc_Minus])
		{
			SD_PlaySound("world/hitwall");
			if (viewsize < 21)
				DrawPlayScreen();
			changeSize = false;
		}
	}
	else if(!Keyboard[sc_Equals] && !Keyboard[sc_Minus])
		changeSize = true;

	if(Keyboard[sc_Alt] && Keyboard[sc_Enter])
		VL_ToggleFullscreen();

//
// F1-F7/ESC to enter control panel
//
	if (scan == sc_F9 || scan == sc_F7 || scan == sc_F8)
	{
		ClearSplitVWB ();
		US_ControlPanel (scan);

		DrawPlayBorderSides ();

		IN_ClearKeysDown ();

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
		if(!startgame && !loadedgame)
			OF_WolfReturnToGameFrame();
		else
#endif
		if(screenfaded && Net::IsBlocked())
			PlayFrame();
		return;
	}

	// bt_esc (the Start button) is reported as a LEVEL while held, so require
	// a fresh press: without the held check, pressing Start inside the menu
	// closed it (via the synthesized Escape key edge) and the still-held
	// button instantly reopened it -- seen as the menu "flashing".  With it,
	// Start cleanly toggles the menu open and closed.
	if ((scan >= sc_F1 && scan <= sc_F9) || scan == sc_Escape ||
		(control[ConsolePlayer].buttonstate[bt_esc] &&
		 !control[ConsolePlayer].buttonheld[bt_esc]))
	{
		int lastoffs = StopMusic ();
		SD_StopDigitized();

		US_ControlPanel (control[ConsolePlayer].buttonstate[bt_esc] ? sc_Escape : scan);

		IN_ClearKeysDown ();
		// The menu may have just been closed with Start; mark bt_esc as still
		// pressed so the next control poll records it as held and a held
		// button cannot re-trigger the menu until it is released.
		control[ConsolePlayer].buttonstate[bt_esc] = true;

		if(screenfaded)
		{
			if (!startgame && !loadedgame)
			{
				VW_FadeOut();
				ContinueMusic (lastoffs);
				if(viewsize != 21)
					DrawPlayScreen ();
			}
			if (loadedgame)
				playstate = ex_abort;
			if (MousePresent && IN_IsInputGrabbed())
				IN_CenterMouse();     // Clear accumulated mouse movement

			// If another player is blocking the play sim we may need to refresh
			// the frame now before we wait for input.
#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
			if(!startgame && !loadedgame)
				OF_WolfReturnToGameFrame();
			else
#endif
			if (Net::IsBlocked())
				PlayFrame();
		}
		else
		{
			ContinueMusic (lastoffs);
#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
			if(!startgame && !loadedgame)
				OF_WolfReturnToGameFrame();
#endif
		}
		return;
	}

	if(scan == sc_F11)
	{
		BumpGamma();
		return;
	}
}


//===========================================================================

/*
=============================================================================

												MUSIC STUFF

=============================================================================
*/


/*
=================
=
= StopMusic
=
=================
*/
int StopMusic (void)
{
	return SD_MusicOff();
}

//==========================================================================


/*
=================
=
= StartMusic
=
=================
*/

void StartMusic ()
{
#if !defined(OF_ECWOLF_OPENFPGA) || defined(OF_PC)
	SD_MusicOff ();
#endif
	SD_StartMusic(levelInfo->GetMusic(map));
}

void ContinueMusic (int offs)
{
#if !defined(OF_ECWOLF_OPENFPGA) || defined(OF_PC)
	SD_MusicOff ();
#endif
	if(!(Paused & 1))
		SD_ContinueMusic(levelInfo->GetMusic(map), offs);
}

/*
=============================================================================

										PALETTE SHIFTING STUFF

=============================================================================
*/

#define NUMREDSHIFTS    6
#define REDSTEPS        8

#define NUMWHITESHIFTS  3
#define WHITESTEPS      20
#define WHITETICS       6

int damagecount, bonuscount;
bool palshifted;

/*
=====================
=
= ClearPaletteShifts
=
=====================
*/

void ClearPaletteShifts (void)
{
	bonuscount = damagecount = 0;
	palshifted = false;
}


/*
=====================
=
= StartBonusFlash
=
=====================
*/

void StartBonusFlash (void)
{
	bonuscount = NUMWHITESHIFTS * WHITETICS;    // white shift palette
}


/*
=====================
=
= StartDamageFlash
=
=====================
*/

void StartDamageFlash (int damage)
{
	damagecount += damage;
}


/*
=====================
=
= UpdatePaletteShifts
=
=====================
*/

void UpdatePaletteShifts (void)
{
	int red, white;

	if (bonuscount)
	{
		white = bonuscount / WHITETICS + 1;
		if (white > NUMWHITESHIFTS)
			white = NUMWHITESHIFTS;
		bonuscount -= tics;
		if (bonuscount < 0)
			bonuscount = 0;
	}
	else
		white = 0;


	if (damagecount)
	{
		red = damagecount / 10 + 1;
		if (red > NUMREDSHIFTS)
			red = NUMREDSHIFTS;

		damagecount -= tics;
		if (damagecount < 0)
			damagecount = 0;
	}
	else
		red = 0;

	if (red)
	{
		V_SetBlend(RPART(players[ConsolePlayer].mo->damagecolor),
                             GPART(players[ConsolePlayer].mo->damagecolor),
                             BPART(players[ConsolePlayer].mo->damagecolor), red*(174/NUMREDSHIFTS));
		palshifted = true;
	}
	else if (white)
	{
		// [BL] More of a yellow if you ask me.
		V_SetBlend(0xFF, 0xF8, 0x00, white*(38/NUMWHITESHIFTS));
		palshifted = true;
	}
	else if (palshifted)
	{
		V_SetBlend(0, 0, 0, 0);
		palshifted = false;
	}
}


/*
=====================
=
= FinishPaletteShifts
=
= Resets palette to normal if needed
=
=====================
*/

void FinishPaletteShifts (void)
{
	damagecount = 0;
	bonuscount = 0;

	if (palshifted)
	{
		V_SetBlend(0, 0, 0, 0);
		VH_UpdateScreen();
		palshifted = false;
	}
}


/*
=============================================================================

												CORE PLAYLOOP

=============================================================================
*/

/*
===================
=
= PlayFrame
=
===================
*/

void PlayFrame()
{
	UpdateRenderInterpolation();
	UpdatePaletteShifts ();

	uint32_t perfStart = OF_WolfPerf_NowUS();
	ThreeDRefresh ();
	OF_WolfPerf_Add(OF_WOLF_PERF_RENDER, perfStart);

	perfStart = OF_WolfPerf_NowUS();
	if((automap && !gamestate.victoryflag) || (Paused & 1) ||
		Net::IsBlocked() || (!loadedgame && viewsize != 21) || screenfaded)
	{
		OF_WolfGPU_FallbackToCPU();
	}

	if(automap && !gamestate.victoryflag)
		BasicOverhead();
	if(Paused & 1)
		VWB_DrawGraphic(TexMan("PAUSED"), (20 - 4)*8, 80 - 2*8);

	if(Net::IsBlocked())
	{
		ClearSplitVWB();
		Message("Waiting for players to return");
	}

	if (!loadedgame)
	{
		StatusBar->Tick();
		if ((gamestate.TimeCount & 1) || !(tics & 1))
			StatusBar->DrawStatusBar();
	}

	if (screenfaded)
	{
		VW_FadeIn ();
		ResetTimeCount();
	}
	OF_WolfPerf_Add(OF_WOLF_PERF_OVERLAY, perfStart);

	/* Reacquire immediately: with the display's one-swap-per-refresh FIFO
	 * flip semantics the buffer acquire is where the loop blocks until the
	 * previous swap is consumed.  Blocking here, at the end of the frame,
	 * means the next frame's input poll, simulation and interpolation
	 * timestamps are all taken on a fresh post-vsync time base instead of
	 * being skewed by a mid-frame stall inside the renderer. */
	VH_UpdateScreen(true);
}

/*
===================
=
= PlayLoop
=
===================
*/
int32_t funnyticount;


void PlayLoop (void)
{
#if 0 // USE_CLOUDSKY
	if(GetFeatureFlags() & FF_CLOUDSKY)
		InitSky();
#endif

	playstate = ex_stillplaying;
	ResetTimeCount();
	frameon = 0;
	funnyticount = 0;
	memset (control[ConsolePlayer].buttonstate, 0, sizeof (control[ConsolePlayer].buttonstate));
	ClearPaletteShifts ();

	if(automap != AMA_Off)
	{
			// Force the automap to off if it were previously on, unpause the game if am_pause
		automap = AMA_Off;

		if(am_pause) Paused &= ~2;
	}


	if (MousePresent && IN_IsInputGrabbed())
		IN_CenterMouse();         // Clear accumulated mouse movement

	if (demoplayback)
		IN_StartAck (ACK_Local);

	StatusBar->NewGame();
	AActor::SyncRenderStates();
	SyncPlayerRenderStates();
	UseCurrentRenderTime();

	do
	{
		OF_WolfPerf_FrameStart();
		uint32_t perfStart = OF_WolfPerf_NowUS();
		ProcessEvents();
		OF_WolfPerf_Add(OF_WOLF_PERF_EVENTS, perfStart);
		OF_WolfPerf_AddTicks(tics);

//
// actor thinking
//
		madenoise = false;

		// Run tics
		perfStart = OF_WolfPerf_NowUS();
		for (unsigned int i = 0;i < tics;++i)
		{
			if(!Paused)
			{
				const uint32_t snapshotStart = OF_WolfPerf_NowUS();
				AActor::SnapshotRenderStates();
				SnapshotPlayerRenderStates();
				OF_WolfPerf_Add(OF_WOLF_PERF_SIM_SNAPSHOT, snapshotStart);
			}
			uint32_t ticPartStart = OF_WolfPerf_NowUS();
			PollControls(!i);
			OF_WolfPerf_Add(OF_WOLF_PERF_SIM_CONTROLS, ticPartStart);

			// Net code may require this loop to abort early
			if(playstate != ex_stillplaying)
				break;

			if(!Paused)
			{
				++gamestate.TimeCount;

				if(AnyPlayerNeedsSpawn())
				{
					ticPartStart = OF_WolfPerf_NowUS();
					CheckSpawnPlayer();
					OF_WolfPerf_Add(OF_WOLF_PERF_SIM_SPAWN, ticPartStart);
				}

				// In single player if the player dies only tick the pawn
				ticPartStart = OF_WolfPerf_NowUS();
				if(Net::InitVars.mode != Net::MODE_SinglePlayer || players[0].state != player_t::PST_DEAD)
					thinkerList.Tick();
				else
					thinkerList.Tick(ThinkerList::PLAYER);
				OF_WolfPerf_Add(OF_WOLF_PERF_SIM_THINKERS, ticPartStart);

				ticPartStart = OF_WolfPerf_NowUS();
				AActor::FinishSpawningActors();
				OF_WolfPerf_Add(OF_WOLF_PERF_SIM_FINISH, ticPartStart);

				ticPartStart = OF_WolfPerf_NowUS();
				GC::CheckGC();
				OF_WolfPerf_Add(OF_WOLF_PERF_SIM_GC, ticPartStart);
			}
		}
		OF_WolfPerf_Add(OF_WOLF_PERF_SIM, perfStart);

		PlayFrame();

		//
		// MAKE FUNNY FACE IF BJ DOESN'T MOVE FOR AWHILE
		//
		perfStart = OF_WolfPerf_NowUS();
		funnyticount += tics;

		TexMan.UpdateAnimations(lasttimecount*14);
		GC::CheckGC();
		OF_WolfPerf_Add(OF_WOLF_PERF_MAINT, perfStart);

		perfStart = OF_WolfPerf_NowUS();
		UpdateSoundLoc ();      // JAB
		OF_WolfPerf_Add(OF_WOLF_PERF_SOUND, perfStart);

		perfStart = OF_WolfPerf_NowUS();
		CheckKeys ();
		CheckDebugKeys ();
		OF_WolfPerf_Add(OF_WOLF_PERF_MAINT, perfStart);

//
// debug aids
//
		if (singlestep)
		{
			VW_WaitVBL (singlestep);
			ResetTimeCount();
		}
		if (extravbls)
			VW_WaitVBL (extravbls);

		if (demoplayback)
		{
			if (IN_CheckAck ())
			{
				IN_ClearKeysDown ();
				playstate = ex_abort;
			}
		}
		OF_WolfPerf_FrameEnd();
	}
	while (!playstate && !startgame);

	if (playstate != ex_died)
		FinishPaletteShifts ();
}
