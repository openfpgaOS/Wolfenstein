/*
** thinker.cpp
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
** Thinkers are given priorities, one of which (TRAVEL) allows us to transfer
** actors between levels without it being collected.  This is similar to ZDoom's
** system, which in turn, is supposedly based off build.
**
*/

#include "farchive.h"
#include "thinker.h"
#include "of_ecwolf_gpu.h"

#if OF_ECWOLF_PERF_ENABLED
extern "C" void OF_WolfPerf_ThinkerSample(const void *key, const char *name,
                                          uint32_t us);
#endif
#include "thingdef/thingdef.h"
#include "wl_def.h"
#include "wl_game.h"
#include "wl_loadsave.h"

ThinkerList thinkerList;

ThinkerList::ThinkerList() : nextThinker(NULL)
{
}

ThinkerList::~ThinkerList()
{
	DestroyAll(static_cast<Priority>(0));
}

void ThinkerList::DestroyAll(Priority start)
{
	for(unsigned int i = start;i < NUM_TYPES;++i)
	{
		Iterator iter = thinkers[i].Head();
		while(iter)
		{
			Thinker *thinker = iter++;

			if(!(thinker->ObjectFlags & OF_EuthanizeMe))
				thinker->Destroy();
		}
	}
#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
	// The dormant side list holds NORMAL-priority thinkers.
	if(start <= NORMAL)
	{
		Iterator iter = dormant.Head();
		while(iter)
		{
			Thinker *thinker = iter++;

			if(!(thinker->ObjectFlags & OF_EuthanizeMe))
				thinker->Destroy();
		}
	}
#endif
	GC::FullGC();
}

void ThinkerList::MarkRoots()
{
	for(unsigned int i = 0;i < NUM_TYPES;++i)
	{
		Iterator iter(thinkers[i]);
		while(iter.Next())
		{
			Thinker *thinker = iter;
			if(!(thinker->ObjectFlags & OF_EuthanizeMe))
			{
				GC::Mark(thinker);
				break;
			}
		}
	}
#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
	// The dormant side list is a GC root chain of its own; marking its
	// first live node lets PropagateMark reach the rest via elNext/elPrev.
	{
		Iterator iter(dormant);
		while(iter.Next())
		{
			Thinker *thinker = iter;
			if(!(thinker->ObjectFlags & OF_EuthanizeMe))
			{
				GC::Mark(thinker);
				break;
			}
		}
	}
#endif
}

void ThinkerList::Tick()
{
	for(unsigned int i = FIRST_TICKABLE;i < NUM_TYPES;++i)
	{
		if(gamestate.victoryflag && i > VICTORY)
			break;

		Tick(static_cast<Priority>(i));
	}
}

void ThinkerList::Tick(Priority list)
{
	Iterator iter = thinkers[list].Head();
	while(iter)
	{
		Thinker *thinker = iter;
		nextThinker = ++iter;

		if(thinker->ObjectFlags & OF_JustSpawned)
		{
			thinker->ObjectFlags &= ~OF_JustSpawned;
			thinker->PostBeginPlay();
		}

		if(!(thinker->ObjectFlags & OF_EuthanizeMe))
		{
#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
			// Skip static objects (decorations, idle pickups, patrol
			// points): they rest on an infinite frame with no thinker, so
			// ticking them is wasted work.  SetState wakes them if anything
			// changes their state.
			if(!thinker->ofThinkDormant)
			{
				thinker->Tick();
				// The tick just made it dormant: divert it to the side
				// list so this walk stops visiting it entirely (the flag
				// skip alone still paid a pointer chase + flag load per
				// dormant node per tic).  NORMAL only, to keep Deregister
				// dispatch and serialization simple; other priorities are
				// rare and stay flag-skipped.
				// Guards: a self-destroyed thinker was already deregistered
				// (euthanize check); a thinker that changed its own priority
				// during Tick (A_BossDeath -> deathcam SetPriority(VICTORY))
				// is now linked in ANOTHER list, so removing it from
				// thinkers[NORMAL] would corrupt both list heads (priority
				// check); one that deactivated without destroying is
				// unlinked entirely (IsLinked check).
				if(thinker->ofThinkDormant && list == NORMAL &&
					thinker->thinkerPriority == NORMAL &&
					!(thinker->ObjectFlags & OF_EuthanizeMe) &&
					EmbeddedList<Thinker>::List::IsLinked(thinker))
				{
					MoveToDormant(thinker);
				}
			}
#else
			thinker->Tick();
			GC::CheckGC();
#endif
		}

		iter = nextThinker;
	}
}

void ThinkerList::Serialize(FArchive &arc)
{
	if(arc.IsStoring())
	{
		for(unsigned int i = 0;i < NUM_TYPES;i++)
		{
			Iterator iter(thinkers[i]);
			while(iter.Next())
			{
				Thinker *thinker = iter;
				arc << thinker;
			}

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
			// Dormant thinkers are all NORMAL priority; store them inside
			// the NORMAL section so loads (which Register into the section
			// index, clearing the transient flag) rebuild identically --
			// they re-divert on their first tick.
			if(i == NORMAL)
			{
				Iterator diter(dormant);
				while(diter.Next())
				{
					Thinker *thinker = diter;
					arc << thinker;
				}
			}
#endif

			Thinker *terminator = NULL;
			arc << terminator; // Terminate list
		}
	}
	else
	{
		for(unsigned int i = 0;i < NUM_TYPES;i++)
		{
			Thinker *thinker;
			arc << thinker;
			while(thinker)
			{
				// FIXME: Remove this save compat hack in 1.4
				if(thinker->IsThinkerType<AActorProxy>())
				{
					Thinker *real = ((AActorProxy*)thinker)->actualObject;
					thinker->Destroy();
					thinker = real;
				}
				Register(thinker, static_cast<Priority>(i));
				arc << thinker;
			}
		}
	}
}

void ThinkerList::Register(Thinker *thinker, Priority type)
{
	thinkers[type].Push(thinker);
	thinker->thinkerPriority = type;
#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
	// Every (re)registration targets a tick list -- keep the dormant-list
	// membership invariant (flag && NORMAL <=> in dormant) intact even
	// across SetPriority and save-game loads.
	thinker->ofThinkDormant = false;
#endif

	Iterator head(thinker);
	if(head.Next())
	{
		GC::WriteBarrier(thinker, head);
		GC::WriteBarrier(head, thinker);
	}
	GC::WriteBarrier(thinker);
}

void ThinkerList::Deregister(Thinker *thinker)
{
	Thinker * const prev = static_cast<Thinker*>(thinker->elPrev);
	Thinker * const next = static_cast<Thinker*>(thinker->elNext);

	// If we're about to think this thinker then we should probably skip it.
	if(nextThinker == thinker)
		nextThinker = next;

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
	// Dormant NORMAL thinkers live in the side list, not thinkers[NORMAL].
	if(thinker->ofThinkDormant && thinker->thinkerPriority == NORMAL)
		dormant.Remove(thinker);
	else
#endif
	thinkers[thinker->thinkerPriority].Remove(thinker);
	if(prev && next)
	{
		GC::WriteBarrier(prev, next);
		GC::WriteBarrier(next, prev);
	}
}

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
void ThinkerList::MoveToDormant(Thinker *thinker)
{
	Thinker * const prev = static_cast<Thinker*>(thinker->elPrev);
	Thinker * const next = static_cast<Thinker*>(thinker->elNext);

	thinkers[NORMAL].Remove(thinker);
	if(prev && next)
	{
		GC::WriteBarrier(prev, next);
		GC::WriteBarrier(next, prev);
	}

	dormant.Push(thinker);

	// Same GC bookkeeping as Register: the new head links form a fresh
	// reference chain for PropagateMark.
	Iterator head(thinker);
	if(head.Next())
	{
		GC::WriteBarrier(thinker, head);
		GC::WriteBarrier(head, thinker);
	}
	GC::WriteBarrier(thinker);
}

void ThinkerList::WakeDormant(Thinker *thinker)
{
	// Only NORMAL-priority thinkers are ever diverted; others carry the
	// (already cleared) flag while sitting in their own tick list.
	if(thinker->thinkerPriority != NORMAL)
		return;
	// Stale-flag safety: an actor that destroyed itself during its Tick can
	// carry the flag without being linked anywhere -- never resurrect it.
	if(thinker->ObjectFlags & OF_EuthanizeMe)
		return;
	if(!EmbeddedList<Thinker>::List::IsLinked(thinker))
		return;

	Thinker * const prev = static_cast<Thinker*>(thinker->elPrev);
	Thinker * const next = static_cast<Thinker*>(thinker->elNext);
	dormant.Remove(thinker);
	if(prev && next)
	{
		GC::WriteBarrier(prev, next);
		GC::WriteBarrier(next, prev);
	}

	Register(thinker, thinker->thinkerPriority);
}
#endif

////////////////////////////////////////////////////////////////////////////////

IMPLEMENT_ABSTRACT_CLASS(Thinker)

Thinker::Thinker(ThinkerList::Priority priority)
{
	ofThinkDormant = false;
	Activate(priority);
}

void Thinker::Activate(ThinkerList::Priority priority)
{
	thinkerList.Register(this, priority);
}

void Thinker::Deactivate()
{
	thinkerList.Deregister(this);
}

void Thinker::Destroy()
{
	if(IsThinking())
		thinkerList.Deregister(this);
	Super::Destroy();
}

void Thinker::Init()
{
	Super::Init();
	ofThinkDormant = false;
	EmbeddedList<Thinker>::List::ValidateNode(this);
}

size_t Thinker::PropagateMark()
{
	if(IsThinking())
	{
		Thinker *next = static_cast<Thinker*>(elNext);
		Thinker *prev = static_cast<Thinker*>(elPrev);
		if(next)
		{
			assert(!(next->ObjectFlags & OF_EuthanizeMe));
			GC::Mark(next);
		}

		if(prev)
		{
			assert(!(prev->ObjectFlags & OF_EuthanizeMe));
			GC::Mark(prev);
		}
	}
	return Super::PropagateMark();
}

void Thinker::Serialize(FArchive &arc)
{
	if(GameSave::SaveVersion > 1451884199)
	{
		BYTE priority = thinkerPriority;
		arc << priority;
		thinkerPriority = static_cast<ThinkerList::Priority>(priority);
	}
	else
		thinkerPriority = ThinkerList::NORMAL;

	Super::Serialize(arc);
}

void Thinker::SetPriority(ThinkerList::Priority priority)
{
	Deactivate();
	Activate(priority);
}
