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