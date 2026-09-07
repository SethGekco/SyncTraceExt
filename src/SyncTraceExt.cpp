#include <Phobos.h>
#include <Syringe.h>

// No init hooks: every well-known init address (ExeRun, CmdLineParse) is
// contested by five-plus frameworks and the first non-zero return in the
// Syringe chain silences the rest. SyncTrace initializes lazily inside its
// own per-frame hook instead, where no init ordering can starve it.

bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID)
{
	if (dwReason == DLL_PROCESS_ATTACH)
		Phobos::hInstance = hInstance;
	return true;
}

SYRINGE_HANDSHAKE(pInfo)
{
	pInfo->Message = const_cast<char*>("SyncTraceExt");
	return S_OK;
}
