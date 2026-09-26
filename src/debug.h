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

// LogMessage, but once per distinct KEY per call site (up to kLogKeyCap keys; after that one
// "suppressed" line). Tells you the whole SET of offending ids rather than only the first.
// Arguments must be side-effect free: they are not evaluated for a key already seen.
constexpr int kLogKeyCap = 64;
#define LOG_ONCE_PER_ID(KEY, FORMAT, ...)                                           \
    do {                                                                            \
        static int aKeys__[kLogKeyCap] = {};                                        \
        static int nKeys__ = 0;                                                     \
        static bool bCapped__ = false;                                              \
        const int nKey__ = (KEY);                                                   \
        bool bSeen__ = false;                                                       \
        for (int i__ = 0; i__ < nKeys__; ++i__) {                                   \
            if (aKeys__[i__] == nKey__) {                                           \
                bSeen__ = true;                                                     \
                break;                                                              \
            }                                                                       \
        }                                                                           \
        if (!bSeen__) {                                                             \
            if (nKeys__ < kLogKeyCap) {                                             \
                aKeys__[nKeys__++] = nKey__;                                        \
                LogMessage(FORMAT, __VA_ARGS__);                                    \
            } else if (!bCapped__) {                                                \
                bCapped__ = true;                                                   \
                LogMessage("    (%d distinct ids logged here; rest suppressed)",    \
                           kLogKeyCap);                                             \
            }                                                                       \
        }                                                                           \
    } while (0)
