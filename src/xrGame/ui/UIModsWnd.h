#pragma once

#include "ModUpdateService.h"
#include "UIDialogWnd.h"
#include "xrUICore/Callbacks/UIWndCallback.h"

class CUI3tButton;
class CUIListBox;
class CUIListBoxItem;
class CUIMessageBoxEx;
class CUIProgressBar;
class CUIScrollView;
class CUIStatic;

// The Mods menu: every XMS module with its name, author and version on the left, the selected
// one in full on the right. Layout and behaviour: docs/dead-air/MOD_UPDATES.md, section 4.
class CUIModsWnd final : public CUIDialogWnd, public CUIWndCallback
{
    using inherited = CUIDialogWnd;

public:
    CUIModsWnd();
    ~CUIModsWnd() override;

    bool Init();
    void Show(bool status) override;
    void Update() override;
    void SendMessage(CUIWindow* window, s16 message, void* data) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboardAction) override;
    pcstr GetDebugType() override { return "CUIModsWnd"; }

private:
    struct Row
    {
        xr_string id;
        // resolved once: a string table id or UTF-8 in the manifest, the UI code page here
        xr_string name;
        xr_string author;
        xr_string description;
        xr_string version;
        bool disabled{};
        bool website{};
        CUIListBoxItem* item{};
        ModUpdateService::State shownState{ModUpdateService::State::NoSource};
    };

    void BuildRows();
    void SelectRow(size_t index);
    void MoveSelection(int step);
    void Refresh(const ModUpdateService::Snapshot& snapshot);
    void RefreshDetails(const Row& row, const ModUpdateService::ModuleStatus& status);
    void AskForRestart();

    void OnUpdate(CUIWindow*, void*);
    void OnUpdateAll(CUIWindow*, void*);
    void OnWebsite(CUIWindow*, void*);
    void OnBack(CUIWindow*, void*);
    void OnRestartYes(CUIWindow*, void*);

    CUIListBox* m_list{};
    CUIStatic* m_empty{};
    CUIStatic* m_name{};
    CUIStatic* m_author{};
    CUIStatic* m_version{};
    CUIStatic* m_status{};
    CUIProgressBar* m_progress{};
    CUIScrollView* m_description{};
    CUIStatic* m_descriptionText{};
    CUI3tButton* m_update{};
    CUI3tButton* m_updateAll{};
    CUI3tButton* m_website{};
    CUI3tButton* m_back{};
    CUIMessageBoxEx* m_restartBox{};

    xr_vector<Row> m_rows;
    size_t m_selected{size_t(-1)};
    size_t m_detailsFor{size_t(-1)};
    // until the player picks a row the selection follows the first module with news
    bool m_autoSelect{true};
    float m_nameWidth{};
    float m_authorWidth{};
    float m_versionWidth{};
    u32 m_normalColor{};
    u32 m_disabledColor{};
    u32 m_updateColor{};
    u32 m_statusColor{};
    u32 m_errorColor{};
};
