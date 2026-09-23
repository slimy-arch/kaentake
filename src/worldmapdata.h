#pragma once
#include "ztl/ztl.h"
#include <string>

// Cached WZ lookups for the world map "Map Info" tooltip (worldmapinfo.cpp).
std::string GetMapNameById(int nMapID);   // "street - map name", or "Unknown"
std::string GetMobNameById(int nMobID);
int GetMobLevelById(int nMobID);
std::string GetNpcNameById(int nNpcID);

IWzCanvasPtr GetMobIcon(int nMobID);
IWzCanvasPtr GetNpcIcon(int nNpcID);
IWzCanvasPtr GetMapMarkIcon(int nMapID);

// 1 = available, 2 = completable, 3 = in progress (UI/UIWindow.img/QuestIcon)
IWzCanvasPtr GetQuestMarkerIcon(int nState);
