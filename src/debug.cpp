#include "pch.h"
#include "debug.h"
#include <windows.h>
#include <strsafe.h>


void DebugMessage(const char* pszFormat, ...) {
    char pszDest[1024];
    size_t cbDest = 1024 * sizeof(char);
    va_list argList;
    va_start(argList, pszFormat);
    StringCbVPrintfA(pszDest, cbDest, pszFormat, argList);
    OutputDebugStringA(pszDest);
    va_end(argList);
}

void ErrorMessage(const char* pszFormat, ...) {
    char pszDest[1024];
    size_t cbDest = 1024 * sizeof(char);
    va_list argList;
    va_start(argList, pszFormat);
    StringCbVPrintfA(pszDest, cbDest, pszFormat, argList);
    MessageBoxA(nullptr, pszDest, "Error", MB_ICONERROR);
    va_end(argList);
}

void LogMessage(const char* pszFormat, ...) {
    char pszDest[1024];
    va_list argList;
    va_start(argList, pszFormat);
    StringCbVPrintfA(pszDest, sizeof(pszDest), pszFormat, argList);
    va_end(argList);

    OutputDebugStringA(pszDest);

    static char sLogPath[MAX_PATH] = {};
    if (!sLogPath[0]) {
        GetModuleFileNameA(nullptr, sLogPath, MAX_PATH);
        char* pLastSlash = strrchr(sLogPath, '\\');
        if (pLastSlash) {
            StringCbCopyA(pLastSlash + 1, sizeof(sLogPath) - (pLastSlash + 1 - sLogPath), "client.log");
        }
    }

    HANDLE hFile = CreateFileA(sLogPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char sLine[1200];
    StringCbPrintfA(sLine, sizeof(sLine), "[%02d:%02d:%02d.%03d] %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, pszDest);
    DWORD dwWritten;
    WriteFile(hFile, sLine, (DWORD)strlen(sLine), &dwWritten, nullptr);
    CloseHandle(hFile);
}
