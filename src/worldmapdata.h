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

// Map ID of the world map spot under (rx, ry), or 0. pDlg is the CWorldMapDlg (IUIMsgHandler this - 4).
// Uses the stock CWorldMapDlg::CheckSpotInfo hit test (worldmapinfo.cpp).
int FindWorldMapSpotMapID(char* pDlg, int rx, int ry);
