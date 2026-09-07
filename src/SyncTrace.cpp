// SyncTrace -- continuous per-frame sync trace + desync-dump preservation.
//
// Problem 1: the engine only dumps state AFTER an out-of-sync is detected,
// which is several frames after the simulations actually diverged. This file
// streams a per-frame line of cheap divergence signals (frame CRC, RNG state,
// object counts) plus periodic per-category object CRCs to SYNCTRACE<N>.LOG.
// Diffing two players' logs (tools/synctrace_diff.py) pinpoints the exact
// first divergent frame and which subsystem moved first.
//
// Problem 2: on desync, Antares (hooking the vanilla log writer 0x64DEA0)
// writes its object-CRC dump to SYNC<N>.TXT -- and Phobos (hooking 0x64736D,
// the very next instruction) then fopen("wt")s the same filename for its
// event-history dump, destroying the Antares dump. We hook the CALL sites of
// 0x64DEA0, let the call happen, then copy SYNC<N>.TXT to SYNCSTATE<N>.TXT
// before Phobos truncates it. Both dumps survive.
//
// Config: SYNCTRACE.INI next to gamemd, section [SyncTrace]:
//   Enabled=1        per-frame trace on/off
//   Cadence=8        full per-category object CRCs every N frames (0 = never)
//   PreserveDumps=1  SYNC->SYNCSTATE copy on/off
// Parsed values are echoed into the trace header so a mis-parse is visible
// (the engine-INI "last line ignored" trap does not apply to
// GetPrivateProfileInt, but echoing config is standing policy anyway).

#include <Utilities/Macro.h>
#include <Syringe.h>

#include <AircraftClass.h>
#include <BuildingClass.h>
#include <CRC.h>
#include <EventClass.h>
#include <HouseClass.h>
#include <InfantryClass.h>
#include <ScenarioClass.h>
#include <UnitClass.h>

#include <Windows.h>
#include <cstdio>
#include <cstring>

namespace SyncTrace
{
	struct Config
	{
		bool Enabled;
		int Cadence;
		bool PreserveDumps;
	};

	// The game's 12-byte CRC object plus one byte of slack: the vanilla
	// Checksummer writes one byte past the end of the staging buffer when
	// finalizing (the bug Ares/Antares patch at 0x4A1C10..0x4A1DE0). With
	// Antares loaded the hooked implementations are safe; without it this
	// padding absorbs the stray write instead of our stack.
	struct SafeCRC
	{
		CRCEngine Engine;
		char Padding;

		SafeCRC()
		{
			std::memset(this, 0, sizeof(*this));
		}

		template<typename T>
		unsigned int OfArray(const DynamicVectorClass<T*>& items)
		{
			for (int i = 0; i < items.Count; ++i)
				items.Items[i]->ComputeCRC(this->Engine);
			return static_cast<unsigned int>(static_cast<int>(this->Engine));
		}
	};

	static Config Cfg;
	static bool Initialized = false;
	static FILE* TraceFile = nullptr;
	static int LastFrame = -1;
	static int LastPlayerIndex = -1;

	static int PlayerIndex()
	{
		auto const pPlayer = HouseClass::CurrentPlayer;
		return pPlayer ? pPlayer->ArrayIndex : 9;
	}

	static void ReadConfig()
	{
		Cfg.Enabled = GetPrivateProfileIntA("SyncTrace", "Enabled", 1, ".\\SYNCTRACE.INI") != 0;
		Cfg.Cadence = GetPrivateProfileIntA("SyncTrace", "Cadence", 8, ".\\SYNCTRACE.INI");
		Cfg.PreserveDumps = GetPrivateProfileIntA("SyncTrace", "PreserveDumps", 1, ".\\SYNCTRACE.INI") != 0;
		if (Cfg.Cadence < 0)
			Cfg.Cadence = 0;
		Initialized = true;
	}

	static void CloseTrace()
	{
		if (TraceFile)
		{
			std::fclose(TraceFile);
			TraceFile = nullptr;
		}
	}

	// (Re)open the trace when a new game starts: the frame counter going
	// backwards, or the local player index changing, means a new session.
	static void EnsureTraceOpen(int frame)
	{
		int const player = PlayerIndex();
		if (TraceFile && (frame < LastFrame || player != LastPlayerIndex))
			CloseTrace();

		if (!TraceFile)
		{
			char name[0x20];
			std::snprintf(name, sizeof(name), "SYNCTRACE%d.LOG", player);
			TraceFile = std::fopen(name, "wt");
			if (TraceFile)
			{
				std::setvbuf(TraceFile, nullptr, _IOFBF, 1 << 16);
				std::fprintf(TraceFile,
					"SyncTrace log; player=%d Enabled=%d Cadence=%d PreserveDumps=%d\n"
					"F=frame CRC=frameCRC RN=rngNext1,rngNext2 RT=rngTableXor "
					"NI/NU/NA/NB/NH=counts CI/CU/CA/CB/CH=categoryCRCs\n",
					player, Cfg.Enabled, Cfg.Cadence, Cfg.PreserveDumps);
			}
			LastPlayerIndex = player;
		}
		LastFrame = frame;
	}

	static void WriteFrameLine(int frame)
	{
		auto const& rng = ScenarioClass::Instance->Random;
		unsigned int tableXor = 0;
		for (auto const entry : rng.Table)
			tableXor ^= entry;

		std::fprintf(TraceFile, "F=%d CRC=%08X RN=%d,%d RT=%08X NI=%d NU=%d NA=%d NB=%d NH=%d",
			frame, static_cast<unsigned int>(EventClass::CurrentFrameCRC),
			rng.Next1, rng.Next2, static_cast<unsigned int>(tableXor),
			InfantryClass::Array.Count, UnitClass::Array.Count,
			AircraftClass::Array.Count, BuildingClass::Array.Count,
			HouseClass::Array.Count);

		if (Cfg.Cadence > 0 && frame % Cfg.Cadence == 0)
		{
			std::fprintf(TraceFile, " CI=%08X CU=%08X CA=%08X CB=%08X CH=%08X",
				SafeCRC().OfArray(InfantryClass::Array),
				SafeCRC().OfArray(UnitClass::Array),
				SafeCRC().OfArray(AircraftClass::Array),
				SafeCRC().OfArray(BuildingClass::Array),
				SafeCRC().OfArray(HouseClass::Array));
		}

		std::fputc('\n', TraceFile);
	}

	static void OnFrame(int frame)
	{
		if (!Initialized)
			ReadConfig();
		if (!Cfg.Enabled)
			return;

		EnsureTraceOpen(frame);
		if (TraceFile)
			WriteFrameLine(frame);
	}

	// Called from the two 0x64DEA0 call sites. Re-invokes the (possibly
	// Antares-hooked) log writer, then snapshots its output file before
	// Phobos's downstream hook truncates it.
	static void PreserveSyncDump(EventClass* pEvent)
	{
		if (!Initialized)
			ReadConfig();

		if (TraceFile)
			std::fflush(TraceFile);

		// One-argument __fastcall places pEvent in ECX, matching the register
		// state at the original call sites (ECX = offending event, or null on
		// the Queue_AI path). Calling the address goes through the Syringe
		// trampoline, so Antares' replacement (or vanilla) runs as usual.
		reinterpret_cast<void(__fastcall*)(EventClass*)>(0x64DEA0)(pEvent);

		if (Cfg.PreserveDumps)
		{
			char src[0x20], dst[0x20];
			int const player = PlayerIndex();
			std::snprintf(src, sizeof(src), "SYNC%d.TXT", player);
			std::snprintf(dst, sizeof(dst), "SYNCSTATE%d.TXT", player);
			CopyFileA(src, dst, FALSE);
		}
	}
}

// Queue_AI multiplayer frame path, immediately after the engine finalizes
// CurrentFrameCRC (call 0x64DAB0 at 0x64731C) and just before it is stored
// into the LatestFramesCRC ring at 0x647334. Runs once per sim frame in MP
// sessions only. Stolen bytes are `mov edx,[0xAC51FC]` -- one whole
// instruction, absolute address, no branch, and we do not write that global,
// so returning 0 and letting the trampoline re-execute it is safe. ECX holds
// the current frame number (loaded from 0xA8ED84 at 0x647321).
DEFINE_HOOK(0x647327, SyncTrace_QueueAI_FrameCRC, 0x6)
{
	GET(const int, frame, ECX);
	SyncTrace::OnFrame(frame);
	return 0;
}

// The two vanilla `call 0x64DEA0` desync-log sites. Each is exactly the
// 5-byte call instruction; a rel32 call must NEVER be re-executed from the
// trampoline copy (unrelocated), so both hooks perform the call themselves
// and return the explicit next address.
//
// Caveat: with Unsorted::EnableMPSyncDebug set, the engine takes the
// 0x6516F0 (per-slot MPDEBUG) path instead and these sites never execute --
// no SYNCSTATE copy in that mode.
DEFINE_HOOK(0x647368, SyncTrace_QueueAI_PreserveSyncDump, 0x5)
{
	GET(EventClass*, pEvent, ECX); // xor'd to null at 0x647366 by the engine
	SyncTrace::PreserveSyncDump(pEvent);
	return 0x64736D;
}

DEFINE_HOOK(0x64CCBA, SyncTrace_ExecuteDoList_PreserveSyncDump, 0x5)
{
	GET(EventClass*, pEvent, ECX);
	SyncTrace::PreserveSyncDump(pEvent);
	return 0x64CCBF;
}
