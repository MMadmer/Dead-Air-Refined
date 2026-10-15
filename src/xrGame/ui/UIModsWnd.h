// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "ModCatalog.h"
#include "ModUpdateService.h"
#include "UIDialogWnd.h"
#include "xrUICore/Callbacks/UIWndCallback.h"
#include "xrUICore/Static/UIShape.h"

#include <unordered_map>

class CUIEditBox;
class CUIModsPrompt;
class CUIModsReview;
class CUIModsRow;
class CUIScrollView;
class CUIStatic;
class CUIXml;

// The Mods menu: the installed modules, and the Mod Browser over the mod catalog - one list and
// one card for both, with the same rating and reviews. Drawn in the game's own look: black
// panels and thin light lines, every one of them computed per pixel (UIShape). Behaviour:
// docs/dead-air/MOD_UPDATES.md 4 and docs/dead-air/MOD_CATALOG.md 14.
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

    // xms_mods <command>: what a click does, for the console and QA; the open window runs it on
    // its next frame. "help" lists the commands.
    static void Queue(pcstr command);

private:
    enum class View : u8
    {
        Installed,
        Browser,
    };
    enum class Sort : u8
    {
        Relevance,
        Rating,
        Newest,
        Name,
        Count,
    };

    struct Item
    {
        xr_string id;
        size_t module{size_t(-1)};             // XMS::Modules() index when on disk
        const ModCatalog::Entry* entry{};      // the catalog's, kept alive by m_catalog
        // UI code page
        xr_string name, author, version, description, mode, tagsText;
        xr_vector<xr_string> tags;
        xr_string thumbnail;
        xr_string website;
        xr_string date;
        // installed from the Mod Browser, then withdrawn by its author: the catalog takes no update of it
        bool withdrawn{};
        // what revokes an installed module its author withdrew (it has no entry); UI code page
        xr_string revoked;
        // folded for the search (ModSearch)
        xr_string foldName, foldAuthor, foldTags, foldDescription;
        float score{};
        bool Installed() const { return module != size_t(-1); }
    };

    void BuildItems();
    void BuildRows();
    void LayoutTags();
    void SetView(View view);
    void Select(size_t index, bool scroll);
    void SelectId(const xr_string& id);
    void MoveSelection(int step);
    const ModUpdateService::ModuleStatus* StatusOf(const Item& item) const;
    void RefreshRows();
    void RefreshDetails();
    void FillDetails(const Item& item);
    void BuildReviews(const Item& item);
    void RunCommand(const xr_string& command);
    void LogState();

    void AskInstall(const Item& item);
    void AskUpdate(const Item& item);
    void AskUpdateAll();
    void AskRemove(const Item& item);
    void AskProtection();
    void AskVirusTotalKey(const xr_string& id);
    void AskRestart();
    void OpenReview(const Item& item);
    void OnReviewSent();

    void OnInstalledTab(CUIWindow*, void*);
    void OnBrowserTab(CUIWindow*, void*);
    void OnProtect(CUIWindow*, void*);
    void OnSort(CUIWindow*, void*);
    void OnTag(CUIWindow*, void*);
    void OnPrimary(CUIWindow*, void*);
    void OnWebsite(CUIWindow*, void*);
    void OnToggle(CUIWindow*, void*);
    void OnRemove(CUIWindow*, void*);
    void OnVtCheck(CUIWindow*, void*);
    void OnVtReport(CUIWindow*, void*);
    void OnWriteReview(CUIWindow*, void*);
    void OnUpdateAll(CUIWindow*, void*);
    void OnBack(CUIWindow*, void*);
    void OnRestart(CUIWindow*, void*);

    const Item* Selected() const { return m_selected < m_items.size() ? &m_items[m_selected] : nullptr; }

    CUIXml* m_xml{};
    CUIModsPrompt* m_prompt{};
    CUIModsReview* m_review{};

    // header and toolbar
    CUIStatic* m_caption{};
    CUIShapeButton* m_tabInstalled{};
    CUIShapeButton* m_tabBrowser{};
    CUIShapeButton* m_protect{};
    // the installed view's summary, the browser's toolbar: the same place
    CUIWindow* m_summary{};
    CUIStatic* m_summaryText{};
    CUIStatic* m_summaryNote{};
    CUIWindow* m_toolbar{};
    CUIEditBox* m_search{};
    CUIStatic* m_searchHint{};
    CUIShapeButton* m_sortButton{};
    CUIStatic* m_count{};
    CUIWindow* m_tagArea{};
    xr_vector<CUIShapeButton*> m_tags;
    // the list
    CUIScrollView* m_lists[2]{};
    CUIStatic* m_listStatus{};
    xr_vector<CUIModsRow*> m_rows;
    // the card
    CUIWindow* m_details{};
    CUIStatic* m_detailsHint{};
    CUIShape* m_thumbFrame{};
    CUIStatic* m_thumb{};
    CUIStatic* m_initial{};
    CUIStatic* m_tagsLine{};
    CUIStatic* m_name{};
    CUIStatic* m_author{};
    CUIStatic* m_meta{};
    CUIShape* m_stars[5]{};
    CUIStatic* m_rating{};
    CUIStatic* m_status{};
    CUIShape* m_progressTrack{};
    CUIShape* m_progressFill{};
    CUIStatic* m_vt{};
    CUIShapeButton* m_vtCheck{};
    CUIShapeButton* m_vtReport{};
    CUIShapeButton* m_primary{};
    CUIShapeButton* m_website{};
    CUIShapeButton* m_toggle{};
    CUIShapeButton* m_remove{};
    CUIScrollView* m_body{};
    CUIShapeButton* m_writeReview{};
    // the foot
    CUIStatic* m_disclaimer{};
    CUIShapeButton* m_updateAll{};
    CUIShapeButton* m_back{};

    // colours the layout names
    u32 m_textColor{}, m_mutedColor{}, m_goodColor{}, m_warnColor{}, m_badColor{}, m_accentColor{};
    float m_progressWidth{};

    View m_view{View::Installed};
    Sort m_sort{Sort::Relevance};
    xr_string m_query;          // UI code page, as typed
    xr_vector<xr_string> m_activeTags;
    xr_vector<Item> m_items;
    size_t m_selected{size_t(-1)};
    xr_string m_selectedIds[2];
    size_t m_detailsFor{size_t(-1)};
    bool m_detailsShown{};
    std::shared_ptr<const ModCatalog::Snapshot> m_catalog;
    u32 m_catalogGeneration{u32(-1)};
    std::shared_ptr<const ModCatalog::Reviews> m_reviewsShown;
    ModUpdateService::Snapshot m_snapshot;
    u64 m_snapshotSignature{};
    bool m_itemsDirty{true};
    // "wait <ms>" holds the console's queue until this time
    u32 m_queueResume{};
    bool m_catalogAsked{};
    // until the player picks a row the installed view follows the first module with news
    bool m_autoSelect{true};
    std::unordered_map<std::string, xr_string> m_installedThumbnails;
    std::unordered_map<std::string, bool> m_enabledNext;
};
