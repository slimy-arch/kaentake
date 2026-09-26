#pragma once

#define CONSTANTS_WINDOW_NAME "Kaentake"
#define CONSTANTS_DLL_NAME    "Kaentake.dll"
#define CONSTANTS_CONFIG_NAME "config.ini"

#define CONSTANTS_CENTER_STATUSBAR FALSE

#define CONSTANTS_DEFAULT_HOST     "127.0.0.1"
#define CONSTANTS_USE_COMMAND_LINE TRUE
#define CONSTANTS_USE_CONFIG_FILE  TRUE

// Monster Book (monsterbook*.cpp). Independent, except SEARCH reads the 16-per-page Dropping paging
// DROPS installs (it guards for it).
#define USE_MONSTER_BOOK_OPEN    TRUE // icons, counters, art/HP/MP, all four tabs
#define USE_MONSTER_BOOK_FOUNDIN TRUE // Found In row click -> world map, text restyling
#define USE_MONSTER_BOOK_DROPS   TRUE // drop chance % label on the Dropping icons
#define USE_MONSTER_BOOK_SEARCH  TRUE // mob-name + item-name search fields

extern char* g_sServerHost;
extern long g_nServerPort;