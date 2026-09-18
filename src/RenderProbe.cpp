// RenderProbe -- confirm (or refute) that the software renderer's dirty-rectangle
// merge is quadratic in on-screen object count.
//
// The profile of a real laggy game put ~28% of all CPU in one place: a shared
// routine at 0x6D2790 that, for a rectangle, scans/merges it against every
// entry already in a global rect list (base 0xB0CE7C, count 0xB0CE88), calling
// the rect-clip helper 0x421B60 per entry. It has 60+ callers across the render
// and object code -- i.e. it is invoked once per invalidating object per frame.
// If each call costs O(current list size N) and N grows to M over a frame, the
// per-frame cost is Sum(0..M) = O(M^2). This probe measures it.
//
// Design: ONE entry hook, zero risk (reads a global, bumps a counter, returns
// 0 to let the stolen bytes run). No timing, no exit hooks (0x6D2B38 is a branch
// target -- unsafe to patch). Instead we histogram the list size N seen at each
// call. The list is rebuilt every frame (grows 0->M), so over a window:
//   * work  = Sum N*hist[N]   (rect comparisons -- the real cost)
//   * M     = max N with hist>0   (scene density proxy)
//   * frames ~ hist at small N    (list resets to ~0 each frame)
// A histogram spread ~flat across [0,M] => work ~ frames*M^2/2 => QUADRATIC.
// A histogram spiked at one N => fixed-size scan => linear. The shape decides.
//
// Off by default. SYNCTRACE.INI [RenderProbe] Enabled=1 turns it on; it appends
// one line per DumpSeconds to RENDERPROBE.CSV next to gamemd.

#include <Utilities/Macro.h>
#include <Syringe.h>

#include <Windows.h>
#include <cstdio>

namespace RenderProbe
{
	static const unsigned HMAX = 8192;   // list sizes above this are clamped

	static bool Initialized = false;
	static bool Enabled = false;
	static DWORD DumpMs = 2000;

	static unsigned Hist[HMAX];
	static unsigned long long Calls;
	static unsigned MaxN;
	static DWORD LastTick;
	static bool HeaderWritten = false;

	static void ReadConfig()
	{
		Enabled = GetPrivateProfileIntA("RenderProbe", "Enabled", 0, ".\\SYNCTRACE.INI") != 0;
		int secs = GetPrivateProfileIntA("RenderProbe", "DumpSeconds", 2, ".\\SYNCTRACE.INI");
		if (secs < 1) secs = 1;
		DumpMs = static_cast<DWORD>(secs) * 1000;
		LastTick = GetTickCount();
		Initialized = true;
	}

	static void Dump()
	{
		FILE* f = nullptr;
		if (fopen_s(&f, "RENDERPROBE.CSV", "at") || !f)
			return;

		if (!HeaderWritten)
		{
			std::fprintf(f, "# dirty-rect merge probe @0x6D2790; per-period rows.\n");
			std::fprintf(f, "# tick,period_calls,maxN,work(sum N*count),hist(N:count ...)\n");
			HeaderWritten = true;
		}

		unsigned long long work = 0;
		for (unsigned n = 0; n < HMAX; ++n)
			work += static_cast<unsigned long long>(n) * Hist[n];

		std::fprintf(f, "%lu,%llu,%u,%llu,", GetTickCount(), Calls, MaxN, work);
		// nonzero bins only, compact
		for (unsigned n = 0; n < HMAX; ++n)
			if (Hist[n])
				std::fprintf(f, "%u:%u ", n, Hist[n]);
		std::fprintf(f, "\n");
		std::fclose(f);

		// reset for the next period
		for (unsigned n = 0; n < HMAX; ++n) Hist[n] = 0;
		Calls = 0;
		MaxN = 0;
	}

	static void OnCall()
	{
		if (!Initialized)
			ReadConfig();
		if (!Enabled)
			return;

		int N = *reinterpret_cast<int*>(0xB0CE88);
		if (N < 0) N = 0;
		unsigned u = (static_cast<unsigned>(N) < HMAX) ? static_cast<unsigned>(N) : HMAX - 1;
		Hist[u]++;
		Calls++;
		if (static_cast<unsigned>(N) > MaxN)
			MaxN = static_cast<unsigned>(N);

		DWORD now = GetTickCount();
		if (now - LastTick >= DumpMs)
		{
			Dump();
			LastTick = now;
		}
	}
}

// Entry of the shared dirty-rect merge routine. Stolen bytes are the whole
// prologue `sub esp,0x58` (3) + `mov ecx,[0xB0CE88]` (6) = 9 bytes -- both
// re-executed by the trampoline after we return 0. We read [0xB0CE88] ourselves
// (the list size N about to be scanned), so we do not depend on ECX. Function
// entry is only reached by `call 0x6D2790`; the 9-byte region is prologue, not
// an internal branch target, so patching it is safe.
DEFINE_HOOK(0x6D2790, RenderProbe_DirtyRectMerge, 0x9)
{
	RenderProbe::OnCall();
	return 0;
}
