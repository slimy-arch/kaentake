#pragma once

#ifdef _DEBUG
#define DEBUG_MESSAGE(FORMAT, ...) DebugMessage(FORMAT, __VA_ARGS__)
#else
#define DEBUG_MESSAGE(FORMAT, ...)
#endif


void DebugMessage(const char* sFormat, ...);

void ErrorMessage(const char* sFormat, ...);

// always-on, writes client.log next to MapleStory.exe (DEBUG_MESSAGE is compiled out in Release)
void LogMessage(const char* sFormat, ...);

// LogMessage, but only the first time this call site is reached.
#define LOG_ONCE(FORMAT, ...)                           do {                                                    static bool bLogged__ = false;                      if (!bLogged__) {                                       bLogged__ = true;                                   LogMessage(FORMAT, __VA_ARGS__);                }                                               } while (0)