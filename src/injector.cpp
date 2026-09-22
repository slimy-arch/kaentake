#include "pch.h"
#include "hook.h"
#include "constants.h"
#include "ztl/ztl.h"
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW - WIN32_LEAN_AND_MEAN keeps it out of windows.h

ZALLOC_GLOBAL
ZALLOCEX(ZAllocAnonSelector, 0x00BF0B00)
ZALLOCEX(ZAllocStrSelector<char>, 0x00BF0A90)
ZALLOCEX(ZAllocStrSelector<wchar_t>, 0x00BF0BA8)

extern "C" __declspec(dllexport) VOID DummyExport() {}

char* g_sServerHost = nullptr;
long g_nServerPort = 0;

void ProcessCommandLine() {
    if (!CONSTANTS_USE_COMMAND_LINE) {
        return;
    }
    // argv[0] is the exe path, so a "%s %d" scan of the raw command line reads that as the host
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return;
    }
    if (argc >= 3) {
        int nPort = _wtoi(argv[2]);
        if (nPort > 0 && nPort < 65536) {
            char sBuffer[256];
            if (WideCharToMultiByte(CP_ACP, 0, argv[1], -1, sBuffer, sizeof(sBuffer), nullptr, nullptr) > 0) {
                g_sServerHost = _strdup(sBuffer);
                g_nServerPort = nPort;
            }
        }
    }
    LocalFree(argv);
}

// ".\config.ini" resolves against the process working directory, which is not the game folder
// when the client is started from anywhere else - resolve an absolute path instead
static void GetConfigPath(char* sIniPath, size_t uCap) {
    sIniPath[0] = '\0';
    if (GetModuleFileNameA(nullptr, sIniPath, (DWORD)uCap) > 0) {
        char* pSlash = strrchr(sIniPath, '\\');
        if (pSlash) {
            *(pSlash + 1) = '\0';
            strcat_s(sIniPath, uCap, CONSTANTS_CONFIG_NAME);
        }
    }
    if (GetFileAttributesA(sIniPath) == INVALID_FILE_ATTRIBUTES) {
        HMODULE hDll = GetModuleHandleA(CONSTANTS_DLL_NAME);
        if (hDll && GetModuleFileNameA(hDll, sIniPath, (DWORD)uCap) > 0) {
            char* pSlash = strrchr(sIniPath, '\\');
            if (pSlash) {
                *(pSlash + 1) = '\0';
                strcat_s(sIniPath, uCap, CONSTANTS_CONFIG_NAME);
            }
        }
    }
    if (GetFileAttributesA(sIniPath) == INVALID_FILE_ATTRIBUTES) {
        strcpy_s(sIniPath, uCap, ".\\" CONSTANTS_CONFIG_NAME);
    }
}

void ProcessConfigFile() {
    if (!CONSTANTS_USE_CONFIG_FILE || g_sServerHost) {
        return;
    }
    char sIniPath[MAX_PATH];
    GetConfigPath(sIniPath, sizeof(sIniPath));

    char sBuffer[1024];
    if (GetPrivateProfileStringA("config", "host", "", sBuffer, sizeof(sBuffer), sIniPath) > 0) {
        g_sServerHost = _strdup(sBuffer);
    }
    if (GetPrivateProfileStringA("config", "port", "", sBuffer, sizeof(sBuffer), sIniPath) > 0) {
        g_nServerPort = atoi(sBuffer);
    }
}


BOOL WINAPI DllMain(HINSTANCE hModule, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        ProcessCommandLine();
        ProcessConfigFile();
        AttachSystemHooks();
        break;
    case DLL_PROCESS_DETACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
