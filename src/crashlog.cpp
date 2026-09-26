#include "pch.h"
#include "hook.h"
#include <intrin.h>


// First-chance crash logger: access violations and thrown C++ exceptions (the client's
// _com_issue_error / ZException) are written to client.log with registers and the likely
// callers inside MapleStory.exe, so a crash can be placed without a debugger. Logging only;
// the exception continues to the client's own handlers.

constexpr DWORD kCppException = 0xE06D7363; // 'msc' — MSVC throw
constexpr uintptr_t kExeTextBegin = 0x00401000;
constexpr uintptr_t kExeTextEnd = 0x00AF0000;
constexpr LONG kMaxLogged = 30;

static LONG CALLBACK CrashLog_Handler(PEXCEPTION_POINTERS pInfo) {
    const EXCEPTION_RECORD* pRecord = pInfo->ExceptionRecord;
    const DWORD dwCode = pRecord->ExceptionCode;
    if (dwCode != EXCEPTION_ACCESS_VIOLATION && dwCode != kCppException) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static LONG s_nLogged = 0;
    if (InterlockedIncrement(&s_nLogged) > kMaxLogged) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const CONTEXT* pCtx = pInfo->ContextRecord;
    char sAccess[64] = "";
    if (dwCode == EXCEPTION_ACCESS_VIOLATION && pRecord->NumberParameters >= 2) {
        sprintf_s(sAccess, " (%s 0x%08X)", pRecord->ExceptionInformation[0] ? "write" : "read", static_cast<unsigned int>(pRecord->ExceptionInformation[1]));
    }

    // Candidate return addresses: stack dwords that point into MapleStory.exe .text.
    char sCallers[160] = "";
    size_t uLen = 0;
    const uintptr_t uStackBase = __readfsdword(4); // NT_TIB::StackBase
    int nFound = 0;
    for (uintptr_t p = pCtx->Esp; p + 4 <= uStackBase && p < pCtx->Esp + 0x400 && nFound < 10; p += 4) {
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
        if (v >= kExeTextBegin && v < kExeTextEnd) {
            uLen += sprintf_s(sCallers + uLen, sizeof(sCallers) - uLen, " %08X", static_cast<unsigned int>(v));
            ++nFound;
        }
    }

    LogMessage("crashlog: %s at %08X%s eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X callers:%s",
            dwCode == kCppException ? "C++ exception" : "access violation",
            static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pRecord->ExceptionAddress)), sAccess,
            pCtx->Eax, pCtx->Ebx, pCtx->Ecx, pCtx->Edx, pCtx->Esi, pCtx->Edi, pCtx->Ebp, pCtx->Esp, sCallers);
    return EXCEPTION_CONTINUE_SEARCH;
}

void AttachCrashLog() {
    AddVectoredExceptionHandler(1, CrashLog_Handler);
}
