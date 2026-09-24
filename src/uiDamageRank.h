#pragma once
#include "pch.h"
#include "wvs/packet.h"
#include <initializer_list>

#include <map>
#include <string>
#include <vector>
#include <windows.h>

class CDamageRankData {
public:
    struct PlayerEntry {
        int charId = 0;
        std::string name;
        int job = 0;
        unsigned long long totalDamage = 0;
    };

    struct SkillEntry {
        int skillId = 0;
        std::string skillName;
        unsigned long long deltaDamage = 0;
        unsigned long long totalDamage = 0;
        unsigned long long maxDamage = 0;
        unsigned long long minDamage = 0;
        unsigned int count = 0;
    };

    static CDamageRankData& GetInstance();

    void Reset();

    void UpsertPlayer(int charId, const std::string& name, int job, unsigned long long totalDamage);
    void UpsertSkill(
            int skillId,
            const std::string& skillName,
            unsigned long long deltaDamage,
            unsigned long long totalDamage,
            unsigned long long maxDamage,
            unsigned long long minDamage,
            unsigned int count);

    std::vector<PlayerEntry> SnapshotPlayers() const;
    std::vector<SkillEntry> SnapshotSkills() const;

private:
    std::map<int, PlayerEntry> m_players;
    std::map<int, SkillEntry> m_skills;
};

class CUIDamageRank {
public:
    enum class Mode {
        Player = 0,
        Skill = 1,
    };

    enum ButtonId : unsigned int {
        kBtnReset = 1003,
        kBtnSwitch = 1004,
        kBtnAuto = 1005,
        kBtnMinMax = 1002,
        kBtnClose = 1000,
    };

    static CUIDamageRank& GetInstance();
    static void ToggleByHotkey();

    static void SendControlOpen();
    static void SendControlReset();
    static void SendControlClose();
    static void PlayUISound(const wchar_t* uol);

    bool EnsureCreated();
    void SetVisible(bool visible);
    bool IsVisible() const;

    void SetMode(Mode mode);
    Mode GetMode() const;

    void Redraw();
    void HandleTrackerPacket(CInPacket* packet);
    bool AnimateTick();

    bool HandleMouseLButtonDown(int screenX, int screenY);
    bool HandleMouseLButtonUp(int screenX, int screenY);
    bool HandleMouseMove(int screenX, int screenY);
    bool HandleMouseWheel(int screenX, int screenY, int wheelDelta);

    bool IsDragging() const { return m_dragging; }

private:
    CUIDamageRank() = default;

    enum class ScrollPart {
        None = 0,
        Prev,
        Next,
        Thumb,
        Track,
    };

    IWzCanvasPtr BuildCanvas();

    void DrawOutlinedText(IWzCanvasPtr canvas, int x, int y, const std::wstring& text);
    void DrawTiledBackground(IWzCanvasPtr canvas);
    void DrawHeader(IWzCanvasPtr canvas);
    void DrawTitleBand(IWzCanvasPtr canvas);
    void DrawPlayerPage(IWzCanvasPtr canvas);
    void DrawSkillPage(IWzCanvasPtr canvas);
    void DrawGaugeBar(IWzCanvasPtr canvas, int x, int y, int width, bool selfStyle, double ratio);

    void StopVectorAnimation(IWzVector2DPtr vector);
    IWzGr2DLayerPtr CreateChildLayerFromCanvas(
            IWzCanvasPtr canvas,
            int x,
            int y,
            int width,
            int height,
            int z);

    IWzCanvasPtr BuildTextCanvas(
            int width,
            int height,
            const std::wstring& text,
            bool outlined);

    void DrawTextCanvasContent(
            IWzCanvasPtr canvas,
            int width,
            int height,
            const std::wstring& text,
            bool selfStyle);

    void UpdateTextLayerCanvas(
            IWzGr2DLayerPtr layer,
            int height,
            const std::wstring& text,
            bool selfStyle);

    void ReplaceLayerCanvas(IWzGr2DLayerPtr layer, IWzCanvasPtr canvas);
    void SetPlayerGaugeLayersVisible(bool visible);
    void ClearPlayerGaugeLayers();
    void SyncPlayerGaugeLayers();

    RECT GetButtonRect(ButtonId id) const;
    bool HitTestHeaderButton(int localX, int localY, ButtonId& outId) const;
    bool HitTestHeaderDragArea(int localX, int localY) const;

    int GetPlayerDisplayRows(size_t playerCount) const;
    void ClampPlayerDisplayRows();

    int GetSkillDisplayRows(size_t skillCount) const;
    void ClampSkillDisplayRows();

    int GetCurrentRollItemCount() const;
    int GetCurrentRollPitch() const;
    int GetCurrentRollRows() const;
    void SetCurrentRollRows(int rows);
    void ClampCurrentRollRows();

    bool HitTestBottomRollArea(int localX, int localY) const;

    int GetCurrentScrollOffset() const;
    void SetCurrentScrollOffset(int offset);
    int GetCurrentMaxScrollOffset() const;
    void ClampCurrentScrollOffset();
    bool HasCurrentScroll() const;
    bool ShouldHideScrollThumb() const;

    RECT GetScrollBarRect() const;
    RECT GetScrollThumbRect() const;
    int GetScrollOffsetFromThumbY(int thumbTop) const;

    bool HitTestScrollBar(int localX, int localY) const;
    bool HitTestScrollThumb(int localX, int localY) const;
    ScrollPart HitTestScrollPart(int localX, int localY) const;

    void DrawScrollBar(IWzCanvasPtr canvas);

    void UpdateBottomRollCursor();
    void UpdateScrollCursor();

    void OnHeaderButton(ButtonId id);
    void UpdateLayerPlacement();
    void UpdateLayerSize();
    int GetCurrentLayerHeight() const;

    bool CreateLayer();
    bool EnsureFonts();
    bool EnsureAssetsLoaded();

    bool LoadCanvasByUol(const char* uol, IWzCanvasPtr& outCanvas);
    void LogCanvasInfo(const char* tag, const IWzCanvasPtr& canvas);
    bool LoadCanvasByAnyUol(
            IWzCanvasPtr& outCanvas,
            std::initializer_list<const char*> uols);

    IWzCanvasPtr GetSkillIconCanvas(int skillId);

    static std::wstring ToWide(const std::string& text);
    static std::wstring FormatComma(unsigned long long value);

    bool IsButtonVisualDisabled(ButtonId id) const;
    bool IsButtonInteractionDisabled(ButtonId id) const;

    bool m_created = false;
    bool m_visible = false;
    bool m_autoMode = true;
    bool m_minimized = false;
    bool m_dragging = false;
    bool m_pressedButtonArmed = false;
    bool m_bottomRollDragging = false;
    bool m_bottomRollHover = false;

    ButtonId m_pressedButton = kBtnReset;

    int m_dragOffsetX = 0;
    int m_dragOffsetY = 0;

    int m_playerDisplayRows = 0;
    int m_skillDisplayRows = 0;

    int m_playerScrollOffset = 0;
    int m_skillScrollOffset = 0;

    bool m_scrollThumbDragging = false;
    bool m_scrollPressedArmed = false;
    int m_scrollThumbGrabOffsetY = 0;

    ScrollPart m_hoveredScrollPart = ScrollPart::None;
    ScrollPart m_pressedScrollPart = ScrollPart::None;

    int m_rollDragStartY = 0;
    int m_rollDragStartRows = 0;
    int m_posX = 520;
    int m_posY = 60;
    Mode m_mode = Mode::Player;

    bool m_assetsLoadTried = false;
    bool m_assetsReady = false;

    IWzGr2DLayerPtr m_layer;
    IWzFontPtr m_fontMain;
    IWzFontPtr m_fontOutline;

    IWzCanvasPtr m_bgMax;
    IWzCanvasPtr m_bgMin;
    IWzCanvasPtr m_bgCenter;
    IWzCanvasPtr m_bgBottom;
    IWzCanvasPtr m_bgBottomOver;

    IWzCanvasPtr m_scrPrev;
    IWzCanvasPtr m_scrNext;
    IWzCanvasPtr m_scrBase;
    IWzCanvasPtr m_scrThumb;

    IWzCanvasPtr m_scrPrevPressed;
    IWzCanvasPtr m_scrNextPressed;
    IWzCanvasPtr m_scrThumbPressed;

    IWzCanvasPtr m_scrPrevMouseOver;
    IWzCanvasPtr m_scrNextMouseOver;
    IWzCanvasPtr m_scrThumbMouseOver;

    IWzCanvasPtr m_title1;
    IWzCanvasPtr m_title2;

    IWzCanvasPtr m_gaugeCommonL;
    IWzCanvasPtr m_gaugeCommonC;
    IWzCanvasPtr m_gaugeCommonR;

    IWzCanvasPtr m_gaugeSelfL;
    IWzCanvasPtr m_gaugeSelfC;
    IWzCanvasPtr m_gaugeSelfR;

    IWzCanvasPtr m_iconCommonAtk;
    IWzCanvasPtr m_iconUnknownSkill;
    std::map<int, IWzCanvasPtr> m_skillIconCache;

    struct PlayerAnimState {
        unsigned long long displayDamage = 0;
        double displayRatio = 0.0;
        double targetRatio = 0.0;
    };

    std::map<int, PlayerAnimState> m_playerAnim;
    DWORD m_lastAnimTick = 0;

    struct PlayerGaugeLayerState {
        IWzGr2DLayerPtr gaugeL;
        IWzGr2DLayerPtr gaugeC;
        IWzGr2DLayerPtr gaugeR;
        IWzGr2DLayerPtr nameText;
        IWzGr2DLayerPtr damageText;

        int targetY = -1;
        int targetWidth = -1;
        int lastDamageWidth = 0;
        long animEndTime = 0;

        bool hasStyle = false;
        bool selfStyle = false;
    };

    std::map<int, PlayerGaugeLayerState> m_playerGaugeLayers;

    IWzCanvasPtr m_btnReset;
    IWzCanvasPtr m_btnSwitch;
    IWzCanvasPtr m_btnAuto;
    IWzCanvasPtr m_btnMin;
    IWzCanvasPtr m_btnMax;
    IWzCanvasPtr m_btnClose;

    IWzCanvasPtr m_btnResetMouseOver;
    IWzCanvasPtr m_btnResetPressed;

    IWzCanvasPtr m_btnSwitchMouseOver;
    IWzCanvasPtr m_btnSwitchPressed;

    IWzCanvasPtr m_btnAutoMouseOver;
    IWzCanvasPtr m_btnAutoPressed;

    IWzCanvasPtr m_btnMinMouseOver;
    IWzCanvasPtr m_btnMinPressed;

    IWzCanvasPtr m_btnMaxMouseOver;
    IWzCanvasPtr m_btnMaxPressed;

    IWzCanvasPtr m_btnCloseMouseOver;
    IWzCanvasPtr m_btnClosePressed;

    IWzCanvasPtr m_btnResetDisabled;
    IWzCanvasPtr m_btnSwitchDisabled;
    IWzCanvasPtr m_btnAutoDisabled;

    IWzCanvasPtr GetButtonCanvasForDraw(ButtonId id) const;
    void UpdateHoveredButtonFromScreenPoint(int screenX, int screenY);

    bool m_hoveredButtonValid = false;
    ButtonId m_hoveredButton = kBtnReset;


    void DrawHeaderButton(
            IWzCanvasPtr canvas,
            int x,
            int y,
            int w,
            int h,
            const IWzCanvasPtr& icon,
            const wchar_t* fallbackText,
            unsigned int fillColor);
};