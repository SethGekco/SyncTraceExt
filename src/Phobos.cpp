#include <Phobos.h>

// Static member definitions required by Container.h and other Phobos utilities.
// Minimal stub -- we define only what this project needs.

HANDLE Phobos::hInstance = nullptr;

char Phobos::readBuffer[Phobos::readLength];
wchar_t Phobos::wideBuffer[Phobos::readLength];

// Stub implementations of Phobos lifecycle methods we don't use.
void Phobos::CmdLineParse(char**, int) { }
void Phobos::ExeRun() { }
void Phobos::ExeTerminate() { }
