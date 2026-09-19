// RenderProbe + RenderCap -- measure and (optionally) fix the O(N^2) dirty-rect
// merge at 0x6D2790 that the profile pinned as ~28% of CPU in busy games.
//
// The routine is AddDirtyRect(x,y,w,h,flag): for each new rectangle it scans the
// whole existing list (base *(void**)0xB0CE7C, count *(int*)0xB0CE88, stride 20:
// 4 ints + 1 flag byte at +16), removing contained entries (an O(N) rep-movsl
// shift each) and merging/appending. O(N) per call, O(N^2) per frame. Confirmed
// quadratic empirically (renderprobe_fit: p~=2.5).
//
// PROBE  ([RenderProbe] Enabled=1): entry hook histograms the list size N seen
//   per call, dumps RENDERPROBE.CSV every DumpSeconds. Zero behaviour change.
//
// CAP    ([RenderCap] Mode=1 observe / Mode=2 active, Threshold=N): when the
//   list exceeds Threshold, collapse it to ONE rectangle = the bounding box
//   (union) of all entries. The bbox is a SUPERSET of every dirty region, so
//   coverage is preserved -- it only ever over-draws, never under-draws (which
//   would leave visual trails). This bounds N and turns O(N^2) into ~O(N).
//   Mode 1 computes + logs the bbox but does NOT modify the list (validation).
//   Mode 2 actually collapses. Guards bail (leaving the list untouched, i.e.
//   the original behaviour) if any element looks malformed or the bbox is absurd,
//   so a wrong assumption degrades to "slow but correct", never to corruption.
//
// All off by default. Single entry hook, returns 0 so the stolen prologue runs.

#include <Utilities/Macro.h>
#include <Syringe.h>

#include <Windows.h>
#include <climits>
#include <cstdio>

namespace RenderProbe
{
	// --- gamemd globals (verified by disassembly of 0x6D2790) ---
	static int&   ListCount = *reinterpret_cast<int*>(0xB0CE88);
	static char*& ListBase  = *reinterpret_cast<char**>(0xB0CE7C);
	static const int STRIDE = 20;

	struct Elem { int x, y, w, h; unsigned char flag; unsigned char pad[3]; };

	static const unsigned HMAX = 8192;
	static const int SANE = 16384;   // coord/size sanity bound

	static bool Initialized = false;
	// probe
	static bool ProbeOn = false;
	static DWORD DumpMs = 2000;
	static unsigned Hist[HMAX];
	static unsigned long long Calls;
	static unsigned MaxN;
	static DWORD LastTick;
	static bool ProbeHeader = false;
	// cap
	static int CapMode = 0;          // 0 off, 1 observe, 2 active
	static int Threshold = 200;
	static unsigned long long Collapses, ElemsCollapsed;
	static DWORD LastCapLog;

	static void ReadConfig()
	{
		const char* ini = ".\\SYNCTRACE.INI";
		ProbeOn = GetPrivateProfileIntA("RenderProbe", "Enabled", 0, ini) != 0;
		int secs = GetPrivateProfileIntA("RenderProbe", "DumpSeconds", 2, ini);
		if (secs < 1) secs = 1;
		DumpMs = static_cast<DWORD>(secs) * 1000;

		CapMode = GetPrivateProfileIntA("RenderCap", "Mode", 0, ini);
		Threshold = GetPrivateProfileIntA("RenderCap", "Threshold", 200, ini);
		if (Threshold < 8) Threshold = 8;   // never collapse tiny lists

		LastTick = LastCapLog = GetTickCount();
		Initialized = true;
	}

	static void DumpProbe()
	{
		FILE* f = nullptr;
		if (fopen_s(&f, "RENDERPROBE.CSV", "at") || !f) return;
		if (!ProbeHeader) {
			std::fprintf(f, "# dirty-rect merge probe @0x6D2790; per-period rows.\n");
			std::fprintf(f, "# tick,period_calls,maxN,work(sum N*count),hist(N:count ...)\n");
			ProbeHeader = true;
		}
		unsigned long long work = 0;
		for (unsigned n = 0; n < HMAX; ++n) work += static_cast<unsigned long long>(n) * Hist[n];
		std::fprintf(f, "%lu,%llu,%u,%llu,", GetTickCount(), Calls, MaxN, work);
		for (unsigned n = 0; n < HMAX; ++n) if (Hist[n]) std::fprintf(f, "%u:%u ", n, Hist[n]);
		std::fprintf(f, "\n");
		std::fclose(f);
		for (unsigned n = 0; n < HMAX; ++n) Hist[n] = 0;
		Calls = 0; MaxN = 0;
	}

	static void CapLog(const char* what, int nBefore, int x, int y, int w, int h, int flag)
	{
		FILE* f = nullptr;
		if (fopen_s(&f, "RENDERCAP.LOG", "at") || !f) return;
		std::fprintf(f, "%lu mode=%d thr=%d %s N=%d bbox=(%d,%d %dx%d) flag=%d "
			"totalCollapses=%llu totalElems=%llu\n",
			GetTickCount(), CapMode, Threshold, what, nBefore, x, y, w, h, flag,
			Collapses, ElemsCollapsed);
		std::fclose(f);
	}

	// Collapse the list to its bounding box. Returns true if it acted (or would,
	// in observe mode). Never modifies the list unless every element is sane.
	static void MaybeCollapse(int N)
	{
		char* base = ListBase;
		if (!base || N < 2) return;

		int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN, flag = 0;
		for (int i = 0; i < N; ++i) {
			const Elem* e = reinterpret_cast<const Elem*>(base + i * STRIDE);
			int x = e->x, y = e->y, w = e->w, h = e->h;
			if (w < 0 || h < 0 || w > SANE || h > SANE ||
				x < -SANE || y < -SANE || x > SANE || y > SANE) {
				CapLog("SKIP-badelem", N, x, y, w, h, 0);   // bail: leave list as-is
				return;
			}
			if (x < minx) minx = x;
			if (y < miny) miny = y;
			if (x + w > maxx) maxx = x + w;
			if (y + h > maxy) maxy = y + h;
			flag |= e->flag;
		}
		int bw = maxx - minx, bh = maxy - miny;
		if (bw <= 0 || bh <= 0 || bw > SANE || bh > SANE) {
			CapLog("SKIP-badbbox", N, minx, miny, bw, bh, flag);
			return;
		}

		bool rate = (GetTickCount() - LastCapLog) >= 1000;
		if (CapMode >= 2) {
			Elem* e0 = reinterpret_cast<Elem*>(base);
			e0->x = minx; e0->y = miny; e0->w = bw; e0->h = bh;
			e0->flag = static_cast<unsigned char>(flag ? 1 : 0);
			ListCount = 1;
			Collapses++; ElemsCollapsed += N;
			if (rate) { CapLog("COLLAPSED", N, minx, miny, bw, bh, flag); LastCapLog = GetTickCount(); }
		} else {
			Collapses++; ElemsCollapsed += N;
			if (rate) { CapLog("WOULD-collapse", N, minx, miny, bw, bh, flag); LastCapLog = GetTickCount(); }
		}
	}

	static void OnCall()
	{
		if (!Initialized) ReadConfig();

		int N = ListCount;
		if (N < 0) N = 0;

		if (ProbeOn) {
			unsigned u = (static_cast<unsigned>(N) < HMAX) ? static_cast<unsigned>(N) : HMAX - 1;
			Hist[u]++; Calls++;
			if (static_cast<unsigned>(N) > MaxN) MaxN = static_cast<unsigned>(N);
			DWORD now = GetTickCount();
			if (now - LastTick >= DumpMs) { DumpProbe(); LastTick = now; }
		}

		if (CapMode >= 1 && N > Threshold)
			MaybeCollapse(N);
	}
}

// Entry of AddDirtyRect @0x6D2790. Stolen bytes = prologue `sub esp,0x58` (3) +
// `mov ecx,[0xB0CE88]` (6) = 9, both re-run by the trampoline after we return 0.
// We read/modify the list globals ourselves; if CAP active-mode set count to 1,
// the re-executed `mov ecx,[0xB0CE88]` picks up the new count. Function entry is
// reached only via `call 0x6D2790`; the 9-byte prologue is not a branch target.
DEFINE_HOOK(0x6D2790, RenderProbe_DirtyRectMerge, 0x9)
{
	RenderProbe::OnCall();
	return 0;
}
