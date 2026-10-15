// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UIModsWnd.h"

#include "ModSearch.h"
#include "UIDialogHolder.h"
#include "UIHelper.h"
#include "UIModsDialogs.h"
#include "UIXmlInit.h"
#include "xrCore/XMS/xms_catalog.h"
#include "xrCore/XMS/xms_core.h"
#include "xrEngine/xr_input.h"
#include "xrEngine/xr_level_controller.h"
#include "xrUICore/EditBox/UIEditBox.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Static/UIStatic.h"

namespace
{
using ModUpdateService::State;

// by value: an id the table does not know comes back as a temporary
shared_str mods_text(pcstr id) { return StringTable().translate(id); }

xr_string mods_format(pcstr id, auto... args)
{
    string1024 text{};
    xr_sprintf(text, sizeof(text), mods_text(id).c_str(), args...);
    return text;
}

// "3.2 MB", or kilobytes for a release small enough to read as "0.0 MB"
xr_string mods_size(u64 bytes)
{
    if (bytes < 1024 * 1024)
        return mods_format("st_mods_size_kb", static_cast<float>(bytes) / 1024.f);
    return mods_format("st_mods_size_mb", static_cast<float>(bytes) / (1024.f * 1024.f));
}

// The UI draws one byte per character in Windows-1251, and a manifest is UTF-8 unless somebody
// wrote it by hand in the game's own code page - in which case it is already what the UI wants.
xr_string mods_to_ui(const xr_string& text)
{
    const int length = static_cast<int>(text.size());
    const int wideLength = length ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0) : 0;
    if (wideLength <= 0)
        return text;
    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, wide.data(), wideLength);
    const int ansiLength = WideCharToMultiByte(1251, 0, wide.data(), wideLength, nullptr, 0, "?", nullptr);
    if (ansiLength <= 0)
        return text;
    xr_string ansi(static_cast<size_t>(ansiLength), '\0');
    WideCharToMultiByte(1251, 0, wide.data(), wideLength, ansi.data(), ansiLength, "?", nullptr);
    return ansi;
}

// A value that reads like an id is looked up first, the way every other caption in the game
// is; prose is never interned as a key just to find out it is not one.
xr_string mods_display(const xr_string& text)
{
    const bool idLike = !text.empty() && text.size() <= 128 && text.find_first_of(" \t\n") == xr_string::npos;
    if (idLike && StringTable().has_translation(text.c_str()))
        return StringTable().translate(text.c_str()).c_str();
    return mods_to_ui(text);
}

// The UI's text layout breaks a line at the two characters backslash and n - what a string
// table carries - and takes a real line feed for nothing at all.
xr_string mods_multiline(const xr_string& text)
{
    xr_string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        if (c == '\n')
            out += "\\n";
        else if (c != '\r')
            out += c;
    }
    return out;
}

bool mods_in_flight(State state)
{
    return state == State::Queued || state == State::Preparing || state == State::Downloading;
}

bool mods_english() { return 0 == xr_strcmp(StringTable().GetCurrentLanguage().c_str(), "eng"); }

// a card carries every language at once; an English game without English text reads the Russian
xr_string mods_card_text(const xr_string& rus, const xr_string& eng)
{
    return mods_to_ui(mods_english() && !eng.empty() ? eng : rus);
}

xr_vector<xr_string> mods_split_tags(const xr_string& text)
{
    xr_vector<xr_string> tags;
    const auto& vocabulary = ModRelease::TagVocabulary();
    std::string_view rest(text);
    while (!rest.empty())
    {
        const size_t comma = rest.find(',');
        xr_string tag(rest.substr(0, comma));
        rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
        tag.erase(0, tag.find_first_not_of(" \t"));
        tag.erase(tag.find_last_not_of(" \t") + 1);
        xr_strlwr(tag);
        if (std::ranges::find(vocabulary, tag) != vocabulary.end() && std::ranges::find(tags, tag) == tags.end())
            tags.push_back(tag);
    }
    return tags;
}

xr_string mods_tag_label(const xr_string& tag) { return mods_text(("st_mods_tag_" + tag).c_str()).c_str(); }

xr_string mods_tags_text(const xr_vector<xr_string>& tags)
{
    xr_string out;
    for (const xr_string& tag : tags)
        out += (out.empty() ? "" : ", ") + mods_tag_label(tag);
    return out;
}

CUIShape* mods_shape(CUIXml& xml, pcstr path, CUIWindow* parent)
{
    auto* shape = xr_new<CUIShape>();
    CUIXmlInit::InitWindow(xml, path, 0, shape);
    UIShape::Read(xml, path, 0, shape->style);
    shape->SetAutoDelete(true);
    parent->AttachChild(shape);
    return shape;
}

CUIShapeButton* mods_button(CUIXml& xml, pcstr path, CUIWindow* parent)
{
    auto* button = xr_new<CUIShapeButton>();
    CUIXmlInit::InitButton(xml, path, 0, button);
    button->InitLooks(xml, path, 0);
    button->SetAutoDelete(true);
    parent->AttachChild(button);
    return button;
}

CUIShapeScrollView* mods_scroll(CUIXml& xml, pcstr path, CUIWindow* parent)
{
    auto* view = xr_new<CUIShapeScrollView>();
    CUIXmlInit::InitScrollView(xml, path, 0, view);
    view->InitLooks(xml, path, 0);
    view->SetAutoDelete(true);
    parent->AttachChild(view);
    return view;
}

CUIWindow* mods_group(CUIXml& xml, pcstr path, CUIWindow* parent)
{
    auto* group = xr_new<CUIWindow>(path);
    CUIXmlInit::InitWindow(xml, path, 0, group);
    group->SetAutoDelete(true);
    parent->AttachChild(group);
    return group;
}

// the width a caption takes, in layout units
float mods_text_width(CUIStatic* owner, pcstr text)
{
    CGameFont* font = owner->TextItemControl()->GetFont();
    if (!font || !text)
        return 0.f;
    float width = font->SizeOf_(text);
    UI().ClientToScreenScaledWidth(width);
    return width;
}

u32 mods_color(CUIXml& xml, pcstr path, pcstr attribute, u32 fallback)
{
    return UIShape::ParseColor(xml.ReadAttrib(path, 0, attribute, nullptr), fallback);
}

// A module without a picture shows the first letter of its name in its place.
xr_string mods_initial(const xr_string& name)
{
    const size_t first = name.find_first_not_of(" \t\"'");
    if (first == xr_string::npos)
        return {};
    u8 c = static_cast<u8>(name[first]);
    if (c >= 'a' && c <= 'z')
        c = static_cast<u8>(c - ('a' - 'A'));
    else if (c >= 0xE0)
        c = static_cast<u8>(c - 0x20);
    return xr_string(1, static_cast<char>(c));
}

xr_vector<xr_string>& mods_queue()
{
    static xr_vector<xr_string> queue;
    return queue;
}
} // namespace

// One module in the list: its picture, name, author and version, its rating and a badge for
// what is going on with it. A click selects it.
class CUIModsRow final : public CUIWindow
{
public:
    CUIModsRow() : CUIWindow("CUIModsRow") {}

    size_t index{};
    bool selected{};
    SUIShapeStyle looks[UIShape::LookCount];
    CUIShape* back{};
    CUIShape* thumbFrame{};
    CUIStatic* thumb{};
    CUIStatic* initial{};
    CUIStatic* name{};
    CUIStatic* meta{};
    CUIShape* star{};
    CUIStatic* rating{};
    CUIShape* badge{};
    CUIStatic* badgeText{};
    xr_string badgeShown;
    float badgeRight{};
    float badgePadding{};

    void Draw() override
    {
        back->style = looks[selected ? CUIShapeButton::lookChecked :
                CursorOverWindow()   ? CUIShapeButton::lookHighlight :
                                       CUIShapeButton::lookNormal];
        CUIWindow::Draw();
    }

    bool OnMouseDown(int button) override
    {
        if (button != MOUSE_1)
            return CUIWindow::OnMouseDown(button);
        GetMessageTarget()->SendMessage(this, LIST_ITEM_CLICKED, &index);
        return true;
    }

    // the badge's text and colour; its box follows the text's width, right-aligned
    void SetBadge(const xr_string& text, u32 color)
    {
        if (text == badgeShown && badge->style.line == color)
            return;
        badgeShown = text;
        badge->Show(!text.empty());
        badgeText->Show(!text.empty());
        if (text.empty())
            return;
        badgeText->SetText(text.c_str());
        badgeText->SetTextColor(color);
        badge->style.line = color;
        const float width = mods_text_width(badgeText, text.c_str()) + badgePadding * 2.f;
        Fvector2 position = badge->GetWndPos();
        position.x = badgeRight - width;
        badge->SetWndPos(position);
        badge->SetWidth(width);
        badgeText->SetWndPos(position);
        badgeText->SetWidth(width);
    }
};

CUIModsWnd::CUIModsWnd() : CUIDialogWnd(CUIModsWnd::GetDebugType()) { m_bWorkInPause = true; }

CUIModsWnd::~CUIModsWnd()
{
    if (m_prompt && m_prompt->IsShown())
        m_prompt->HideDialog();
    if (m_review && m_review->IsShown())
        m_review->HideDialog();
    xr_delete(m_prompt);
    xr_delete(m_review);
    xr_delete(m_xml);
}

void CUIModsWnd::Queue(pcstr command)
{
    if (command && *command)
        mods_queue().emplace_back(command);
}

bool CUIModsWnd::Init()
{
    m_xml = xr_new<CUIXml>();
    if (!m_xml->Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_mods.xml", false))
        return false;
    CUIXml& xml = *m_xml;

    CUIXmlInit::InitWindow(xml, "main", 0, this);
    m_textColor = mods_color(xml, "main:colors", "text", 0xffece9e2);
    m_mutedColor = mods_color(xml, "main:colors", "muted", 0xff9a968c);
    m_goodColor = mods_color(xml, "main:colors", "good", 0xff7fb069);
    m_warnColor = mods_color(xml, "main:colors", "warn", 0xffd9a441);
    m_badColor = mods_color(xml, "main:colors", "bad", 0xffdc645a);
    m_accentColor = mods_color(xml, "main:colors", "accent", 0xffd2be73);

    mods_shape(xml, "main:shade", this);
    mods_shape(xml, "main:shadow", this);
    mods_shape(xml, "main:frame", this);
    m_caption = UIHelper::CreateStatic(xml, "main:caption", this);
    m_tabInstalled = mods_button(xml, "main:tab_installed", this);
    m_tabBrowser = mods_button(xml, "main:tab_browser", this);
    m_protect = mods_button(xml, "main:protect", this);
    mods_shape(xml, "main:separator", this);

    // the browser's search, sort and tags
    m_toolbar = mods_group(xml, "main:toolbar", this);
    mods_shape(xml, "main:toolbar:search_back", m_toolbar);
    mods_shape(xml, "main:toolbar:search_icon", m_toolbar);
    m_searchHint = UIHelper::CreateStatic(xml, "main:toolbar:search_hint", m_toolbar);
    m_search = UIHelper::CreateEditBox(xml, "main:toolbar:search", m_toolbar);
    m_sortButton = mods_button(xml, "main:toolbar:sort", m_toolbar);
    m_count = UIHelper::CreateStatic(xml, "main:toolbar:count", m_toolbar);
    m_tagArea = mods_group(xml, "main:toolbar:tags", m_toolbar);
    for (const xr_string& tag : ModRelease::TagVocabulary())
    {
        CUIShapeButton* chip = mods_button(xml, "main:toolbar:tags:tag", m_tagArea);
        chip->SetText(mods_tag_label(tag).c_str());
        chip->SetWindowName(tag.c_str());
        Register(chip);
        AddCallback(chip, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIModsWnd::OnTag));
        m_tags.push_back(chip);
    }
    LayoutTags();

    // the installed view's summary in the toolbar's place
    m_summary = mods_group(xml, "main:summary", this);
    m_summaryText = UIHelper::CreateStatic(xml, "main:summary:text", m_summary);
    m_summaryNote = UIHelper::CreateStatic(xml, "main:summary:note", m_summary);
    m_updateAll = mods_button(xml, "main:summary:update_all", m_summary);

    // a list per view in the same place: each keeps its own scroll position
    mods_shape(xml, "main:list_back", this);
    for (CUIScrollView*& list : m_lists)
        list = mods_scroll(xml, "main:list", this);
    m_listStatus = UIHelper::CreateStatic(xml, "main:list_status", this);

    // the card
    m_details = mods_group(xml, "main:details", this);
    mods_shape(xml, "main:details:back", m_details);
    m_detailsHint = UIHelper::CreateStatic(xml, "main:details:hint", m_details);
    m_thumbFrame = mods_shape(xml, "main:details:thumb_frame", m_details);
    m_initial = UIHelper::CreateStatic(xml, "main:details:initial", m_details);
    m_thumb = UIHelper::CreateStatic(xml, "main:details:thumb", m_details);
    m_thumb->SetStretchTexture(true);
    m_name = UIHelper::CreateStatic(xml, "main:details:name", m_details);
    m_author = UIHelper::CreateStatic(xml, "main:details:author", m_details);
    m_meta = UIHelper::CreateStatic(xml, "main:details:meta", m_details);
    m_tagsLine = UIHelper::CreateStatic(xml, "main:details:tags", m_details);
    const float starStep = xml.ReadAttribFlt("main:details:star", 0, "step", 18.f);
    for (u32 star = 0; star != 5; ++star)
    {
        m_stars[star] = mods_shape(xml, "main:details:star", m_details);
        Fvector2 position = m_stars[star]->GetWndPos();
        position.x += starStep * star;
        m_stars[star]->SetWndPos(position);
    }
    m_rating = UIHelper::CreateStatic(xml, "main:details:rating", m_details);
    m_status = UIHelper::CreateStatic(xml, "main:details:status", m_details);
    m_progressTrack = mods_shape(xml, "main:details:progress_track", m_details);
    m_progressFill = mods_shape(xml, "main:details:progress_fill", m_details);
    m_progressWidth = m_progressFill->GetWidth();
    m_vt = UIHelper::CreateStatic(xml, "main:details:vt", m_details);
    m_vtCheck = mods_button(xml, "main:details:vt_check", m_details);
    m_vtReport = mods_button(xml, "main:details:vt_report", m_details);
    m_primary = mods_button(xml, "main:details:primary", m_details);
    m_website = mods_button(xml, "main:details:website", m_details);
    m_toggle = mods_button(xml, "main:details:toggle", m_details);
    m_remove = mods_button(xml, "main:details:remove", m_details);
    mods_shape(xml, "main:details:body_rule", m_details);
    m_body = mods_scroll(xml, "main:details:body", m_details);

    // the foot
    m_disclaimer = UIHelper::CreateStatic(xml, "main:disclaimer", this);
    m_back = mods_button(xml, "main:back", this);

    const std::pair<CUIShapeButton*, void (CUIModsWnd::*)(CUIWindow*, void*)> handlers[] = {
        {m_tabInstalled, &CUIModsWnd::OnInstalledTab}, {m_tabBrowser, &CUIModsWnd::OnBrowserTab},
        {m_protect, &CUIModsWnd::OnProtect}, {m_sortButton, &CUIModsWnd::OnSort}, {m_primary, &CUIModsWnd::OnPrimary},
        {m_website, &CUIModsWnd::OnWebsite}, {m_toggle, &CUIModsWnd::OnToggle}, {m_remove, &CUIModsWnd::OnRemove},
        {m_vtCheck, &CUIModsWnd::OnVtCheck}, {m_vtReport, &CUIModsWnd::OnVtReport},
        {m_updateAll, &CUIModsWnd::OnUpdateAll}, {m_back, &CUIModsWnd::OnBack}};
    for (const auto& [button, handler] : handlers)
    {
        Register(button);
        AddCallback(button, BUTTON_CLICKED, CUIWndCallback::void_function(this, handler));
    }

    m_prompt = xr_new<CUIModsPrompt>();
    m_prompt->Init(xml);
    m_review = xr_new<CUIModsReview>();
    m_review->Init(xml);
    m_review->onSent = CUIModsReview::Callback(this, &CUIModsWnd::OnReviewSent);

    SetView(View::Installed);
    return true;
}

// The chips flow left to right and wrap inside their area.
void CUIModsWnd::LayoutTags()
{
    const float gap = m_xml->ReadAttribFlt("main:toolbar:tags", 0, "gap", 6.f);
    const float padding = m_xml->ReadAttribFlt("main:toolbar:tags", 0, "padding", 10.f);
    const float width = m_tagArea->GetWidth();
    float x = 0.f, y = 0.f;
    for (CUIShapeButton* chip : m_tags)
    {
        const float chipWidth = mods_text_width(chip, chip->GetText()) + padding * 2.f;
        if (x > 0.f && x + chipWidth > width)
        {
            x = 0.f;
            y += chip->GetHeight() + gap;
        }
        chip->SetWndPos(Fvector2().set(x, y));
        chip->SetWidth(chipWidth);
        x += chipWidth + gap;
    }
}

void CUIModsWnd::Show(bool status)
{
    inherited::Show(status);
    if (!status)
        return;
    // a check that failed - no network at startup, say - gets another go whenever the player looks
    ModUpdateService::StartCheck();
    m_autoSelect = true;
    m_itemsDirty = true;
    m_detailsFor = size_t(-1);
    m_enabledNext.clear();
}

void CUIModsWnd::SetView(View view)
{
    m_view = view;
    const bool browser = view == View::Browser;
    m_tabInstalled->checked = !browser;
    m_tabBrowser->checked = browser;
    m_toolbar->Show(browser);
    m_summary->Show(!browser);
    m_disclaimer->Show(browser);
    for (size_t index = 0; index != 2; ++index)
        m_lists[index]->Show(index == static_cast<size_t>(view));
    // the catalog loads when the browser is first looked at; the cached copy shows meanwhile
    if (browser && !m_catalogAsked)
    {
        m_catalogAsked = true;
        ModCatalog::Refresh();
    }
    if (browser)
        m_search->CaptureFocus(true);
    m_itemsDirty = true;
}

void CUIModsWnd::BuildItems()
{
    m_itemsDirty = false;
    m_catalog = ModCatalog::Current();
    m_catalogGeneration = m_catalog->generation;
    const xr_string keep = m_selectedIds[static_cast<size_t>(m_view)];
    m_items.clear();

    const auto folded = [](Item& item)
    {
        item.foldName = ModSearch::Fold(item.name);
        item.foldAuthor = ModSearch::Fold(item.author);
        item.foldTags = ModSearch::Fold(item.tagsText);
        item.foldDescription = ModSearch::Fold(item.description);
    };
    const auto fromCard = [&](Item& item, const ModCatalog::Entry& entry)
    {
        const ModRelease::Card& card = entry.release.card;
        item.name = mods_card_text(card.name, card.nameEng);
        item.author = mods_card_text(card.author, card.authorEng);
        item.description = mods_multiline(mods_card_text(card.description, card.descriptionEng));
        item.version = entry.release.version;
        item.tags = card.tags;
        item.website = card.website;
        item.thumbnail = entry.thumbnailTexture;
        item.date = entry.listed;
        if (card.mode.starts_with("new "))
            item.mode = mods_format("st_mods_mode_new", mods_card_text(card.modeTitle, card.modeTitleEng).c_str());
        else if (card.mode.starts_with("mode "))
            item.mode = mods_format("st_mods_mode_existing", mods_card_text(card.modeTitle, card.modeTitleEng).c_str());
        else
            item.mode = mods_text("st_mods_mode_plain").c_str();
    };

    if (m_view == View::Installed)
    {
        const xr_vector<XMS::Module>& modules = XMS::Modules();
        for (size_t index = 0; index != modules.size(); ++index)
        {
            const XMS::Module& module = modules[index];
            Item& item = m_items.emplace_back();
            item.id = module.id;
            item.module = index;
            // another author's module under a listed id is not the catalog's: no card, rating or reviews
            item.entry = XMS::Catalog::IsCatalogModule(module) ? m_catalog->Find(module.id) : nullptr;
            item.name = mods_display(module.name);
            item.author = mods_display(module.author);
            item.description = mods_multiline(mods_display(module.description));
            item.version = module.version;
            item.tags = mods_split_tags(module.tags);
            item.website = module.website;
            item.mode = module.mode.empty() ? mods_text("st_mods_mode_plain").c_str() :
                module.mode == "*"          ? mods_text("st_mods_mode_any").c_str() :
                                              mods_format("st_mods_mode_existing", module.mode.c_str());
            auto [thumbnail, fresh] = m_installedThumbnails.try_emplace(module.id.c_str());
            if (fresh)
                thumbnail->second = ModCatalog::InstalledThumbnail(module);
            item.thumbnail = thumbnail->second;
            if (item.thumbnail.empty() && item.entry)
                item.thumbnail = item.entry->thumbnailTexture;
            // no card any more, but its id is still bound to its key: a revocation reaches it all the same
            if (XMS::Catalog::Withdrawn(module))
            {
                item.withdrawn = XMS::Catalog::InstalledFromBrowser(module.id.c_str());
                item.revoked = mods_to_ui(XMS::Catalog::RevocationReason(module));
            }
        }
        // what the browser is installing is the player's too, before it is on the disk
        for (const ModUpdateService::ModuleStatus& install : m_snapshot.installs)
        {
            const ModCatalog::Entry* entry = m_catalog->Find(install.id);
            if (!entry || XMS::FindModule(install.id.c_str()))
                continue;
            Item& item = m_items.emplace_back();
            item.id = install.id;
            item.entry = entry;
            fromCard(item, *entry);
        }
    }
    else
    {
        // the folded text outlives the words that point into it
        const xr_string foldedQuery = ModSearch::Fold(m_query);
        xr_vector<xr_string> query;
        for (const std::string_view word : ModSearch::Words(foldedQuery))
            query.emplace_back(word);
        for (const ModCatalog::Entry& entry : m_catalog->entries)
        {
            // what cannot be installed from the browser is not offered by it
            if (!entry.hasCard || !entry.revoked.empty())
                continue;
            Item item;
            item.id = entry.id;
            item.entry = &entry;
            if (const XMS::Module* module = XMS::FindModule(entry.id.c_str()); module && XMS::Catalog::IsCatalogModule(*module))
                item.module = static_cast<size_t>(module - XMS::Modules().data());
            fromCard(item, entry);
            item.tagsText = mods_tags_text(item.tags);
            if (!std::ranges::all_of(m_activeTags, [&](const xr_string& tag) { return std::ranges::find(item.tags, tag) != item.tags.end(); }))
                continue;
            folded(item);
            if (!query.empty())
            {
                item.score = ModSearch::Score(query, {item.foldName, item.foldAuthor, item.foldTags, item.foldDescription});
                if (item.score <= 0.f)
                    continue;
            }
            if (item.thumbnail.empty())
                ModCatalog::WantThumbnail(entry.id.c_str());
            m_items.push_back(std::move(item));
        }

        const auto rating = [](const Item& item) { return item.entry ? item.entry->rating.average100 : 0u; };
        const auto votes = [](const Item& item) { return item.entry ? item.entry->rating.count : 0u; };
        std::ranges::stable_sort(m_items, [&](const Item& a, const Item& b)
        {
            switch (m_sort)
            {
            case Sort::Relevance:
                if (a.score != b.score)
                    return a.score > b.score;
                [[fallthrough]];
            case Sort::Rating:
                if (rating(a) != rating(b))
                    return rating(a) > rating(b);
                if (votes(a) != votes(b))
                    return votes(a) > votes(b);
                break;
            case Sort::Newest:
                if (a.date != b.date)
                    return a.date > b.date;
                break;
            default: break;
            }
            return a.foldName < b.foldName;
        });
    }
    for (Item& item : m_items)
    {
        if (item.tagsText.empty())
            item.tagsText = mods_tags_text(item.tags);
        if (item.foldName.empty())
            folded(item);
    }

    BuildRows();
    m_selected = size_t(-1);
    m_detailsFor = size_t(-1);
    SelectId(keep);
    if (m_selected == size_t(-1) && !m_items.empty())
        Select(0, false);
}

void CUIModsWnd::BuildRows()
{
    CUIScrollView* list = m_lists[static_cast<size_t>(m_view)];
    const int scroll = list->GetCurrentScrollPos();
    list->Clear();
    m_rows.clear();
    CUIXml& xml = *m_xml;
    const float badgeRight = xml.ReadAttribFlt("main:row:badge", 0, "right", 0.f);
    const float badgePadding = xml.ReadAttribFlt("main:row:badge", 0, "padding", 6.f);
    for (size_t index = 0; index != m_items.size(); ++index)
    {
        const Item& item = m_items[index];
        auto* row = xr_new<CUIModsRow>();
        CUIXmlInit::InitWindow(xml, "main:row", 0, row);
        row->SetWidth(list->GetDesiredChildWidth());
        row->index = index;
        row->SetMessageTarget(this);
        u32 unusedText[UIShape::LookCount]{};
        UIShape::ReadLooks(xml, "main:row:back", 0, row->looks, unusedText, 0);
        row->back = mods_shape(xml, "main:row:back", row);
        row->back->SetWidth(row->GetWidth());
        row->thumbFrame = mods_shape(xml, "main:row:thumb_frame", row);
        row->initial = UIHelper::CreateStatic(xml, "main:row:initial", row);
        row->thumb = UIHelper::CreateStatic(xml, "main:row:thumb", row);
        row->thumb->SetStretchTexture(true);
        // a thumbnail name is only handed out once the file is published; InitTexture's answer
        // says whether the name is an atlas entry, which a thumbnail never is
        if (!item.thumbnail.empty())
        {
            row->thumb->InitTexture(item.thumbnail.c_str(), false);
            row->thumb->TextureOn();
            row->initial->Show(false);
        }
        else
        {
            row->thumb->Show(false);
            row->initial->SetText(mods_initial(item.name).c_str());
        }
        const float textRight = row->GetWidth() - xml.ReadAttribFlt("main:row", 0, "text_right", 96.f);
        row->name = UIHelper::CreateStatic(xml, "main:row:name", row);
        row->name->SetWidth(textRight - row->name->GetWndPos().x);
        row->name->SetText(item.name.c_str());
        row->name->SetEllipsis(true);
        row->meta = UIHelper::CreateStatic(xml, "main:row:meta", row);
        row->meta->SetWidth(textRight - row->meta->GetWndPos().x);
        xr_string meta = item.author.empty() ? xr_string() : item.author;
        if (!item.version.empty())
            meta += (meta.empty() ? "" : " \xB7 ") + mods_format("st_mods_version_short", item.version.c_str());
        row->meta->SetText(meta.c_str());
        row->meta->SetEllipsis(true);

        // the rating stands at the right of the name; a module nobody rated shows none
        row->star = mods_shape(xml, "main:row:star", row);
        row->rating = UIHelper::CreateStatic(xml, "main:row:rating", row);
        const float right = row->GetWidth();
        for (CUIWindow* part : {static_cast<CUIWindow*>(row->star), static_cast<CUIWindow*>(row->rating)})
        {
            Fvector2 position = part->GetWndPos();
            position.x += right;
            part->SetWndPos(position);
        }
        const bool rated = item.entry && item.entry->rating.count;
        row->star->Show(rated);
        row->rating->Show(rated);
        if (rated)
        {
            string32 text{};
            xr_sprintf(text, "%.1f", item.entry->rating.average100 / 100.f);
            row->rating->SetText(text);
        }

        row->badge = mods_shape(xml, "main:row:badge", row);
        row->badgeText = UIHelper::CreateStatic(xml, "main:row:badge_text", row);
        row->badgeRight = right - badgeRight;
        row->badgePadding = badgePadding;
        row->badge->Show(false);
        row->badgeText->Show(false);
        list->AddWindow(row, true);
        m_rows.push_back(row);
    }
    list->SetScrollPos(std::clamp(scroll, list->GetMinScrollPos(), std::max(list->GetMinScrollPos(), list->GetMaxScrollPos())));
    m_snapshotSignature = 0;
}

void CUIModsWnd::Select(size_t index, bool scroll)
{
    if (index >= m_items.size())
        return;
    m_selected = index;
    m_selectedIds[static_cast<size_t>(m_view)] = m_items[index].id;
    for (CUIModsRow* row : m_rows)
        row->selected = row->index == index;
    if (scroll && index < m_rows.size())
        m_lists[static_cast<size_t>(m_view)]->ScrollToWindow(m_rows[index]);
}

void CUIModsWnd::SelectId(const xr_string& id)
{
    for (size_t index = 0; index != m_items.size(); ++index)
        if (m_items[index].id == id)
        {
            Select(index, true);
            return;
        }
}

void CUIModsWnd::MoveSelection(int step)
{
    if (m_items.empty())
        return;
    m_autoSelect = false;
    const int last = static_cast<int>(m_items.size()) - 1;
    const int current = m_selected < m_items.size() ? static_cast<int>(m_selected) : (step > 0 ? -1 : last + 1);
    Select(static_cast<size_t>(std::clamp(current + step, 0, last)), true);
}

const ModUpdateService::ModuleStatus* CUIModsWnd::StatusOf(const Item& item) const
{
    if (item.Installed())
        return item.module < m_snapshot.modules.size() ? &m_snapshot.modules[item.module] : nullptr;
    for (const ModUpdateService::ModuleStatus& install : m_snapshot.installs)
        if (install.id == item.id)
            return &install;
    return nullptr;
}

void CUIModsWnd::Update()
{
    inherited::Update();

    // the console's commands wait for a window that draws; "wait <ms>" holds the rest
    xr_vector<xr_string>& queue = mods_queue();
    while (!queue.empty() && Device.dwTimeGlobal >= m_queueResume)
    {
        const xr_string command = queue.front();
        queue.erase(queue.begin());
        RunCommand(command);
    }

    m_snapshot = ModUpdateService::GetSnapshot();
    if (m_view == View::Browser)
    {
        if (0 != xr_strcmp(m_search->GetText(), m_query.c_str()))
        {
            m_query = m_search->GetText();
            m_itemsDirty = true;
        }
        m_searchHint->Show(m_query.empty());
    }
    const std::shared_ptr<const ModCatalog::Snapshot> catalog = ModCatalog::Current();
    if (catalog->generation != m_catalogGeneration)
        m_itemsDirty = true;
    // the installed view lists what the browser is installing
    if (m_view == View::Installed)
    {
        size_t pending = 0;
        for (const ModUpdateService::ModuleStatus& install : m_snapshot.installs)
            pending += !XMS::FindModule(install.id.c_str());
        size_t shown = 0;
        for (const Item& item : m_items)
            shown += !item.Installed();
        m_itemsDirty |= pending != shown;
    }
    if (m_itemsDirty)
        BuildItems();

    RefreshRows();
    RefreshDetails();

    // the header
    const bool protectedMode = XMS::Catalog::ProtectedMode() || XMS::Catalog::ProtectionLatched();
    m_protect->SetText(mods_text(protectedMode ? "st_mods_protect_on" : "st_mods_protect_off").c_str());
    m_protect->checked = protectedMode;
    m_protect->icon = protectedMode ? SUIShapeStyle::Kind::Check : SUIShapeStyle::Kind::Cross;

    // the list's own message: nothing installed, nothing found, the catalog on its way
    xr_string status;
    if (m_view == View::Browser)
    {
        switch (m_catalog->status)
        {
        case ModCatalog::Status::Idle:
        case ModCatalog::Status::Loading:
            if (m_catalog->entries.empty())
                status = mods_text("st_mods_catalog_loading").c_str();
            break;
        case ModCatalog::Status::Failed: status = mods_text("st_mods_catalog_failed").c_str(); break;
        default: break;
        }
        if (status.empty() && m_items.empty())
            status = mods_text("st_mods_empty_search").c_str();
        xr_string count = mods_format("st_mods_count", static_cast<u32>(m_items.size()));
        if (m_catalog->status == ModCatalog::Status::Offline)
            count += mods_text("st_mods_catalog_offline").c_str();
        if (0 != xr_strcmp(m_count->GetText(), count.c_str()))
            m_count->SetText(count.c_str());
        for (CUIShapeButton* chip : m_tags)
            chip->checked = std::ranges::find(m_activeTags, chip->WindowName().c_str()) != m_activeTags.end();
        string128 sort{};
        static constexpr pcstr sorts[] = {"st_mods_sort_relevance", "st_mods_sort_rating", "st_mods_sort_newest", "st_mods_sort_name"};
        xr_strcpy(sort, mods_text(sorts[static_cast<size_t>(m_sort)]).c_str());
        if (0 != xr_strcmp(m_sortButton->GetText(), sort))
            m_sortButton->SetText(sort);
    }
    else if (m_items.empty())
        status = mods_text("st_mods_empty").c_str();
    m_listStatus->Show(!status.empty());
    if (!status.empty() && 0 != xr_strcmp(m_listStatus->GetText(), status.c_str()))
        m_listStatus->SetText(status.c_str());

    // the installed view's summary: how many, what is new, what protection means now
    u32 available = 0, checking = 0;
    for (const ModUpdateService::ModuleStatus& module : m_snapshot.modules)
    {
        available += module.state == State::Available;
        checking += module.state == State::Unchecked || module.state == State::Checking;
    }
    m_updateAll->Enable(available != 0);
    if (m_view == View::Installed)
    {
        const xr_string summary = mods_format("st_mods_summary", static_cast<u32>(XMS::Modules().size()));
        if (0 != xr_strcmp(m_summaryText->GetText(), summary.c_str()))
            m_summaryText->SetText(summary.c_str());
        xr_string note = available ? mods_format("st_mods_summary_updates", available) :
            checking               ? xr_string(mods_text("st_mods_status_checking").c_str()) :
                                     xr_string(mods_text("st_mods_summary_current").c_str());
        note += "\\n";
        note += mods_text(protectedMode ? "st_mods_summary_protected" : "st_mods_summary_unprotected").c_str();
        if (0 != xr_strcmp(m_summaryNote->GetText(), note.c_str()))
            m_summaryNote->SetText(note.c_str());
    }

    if (m_snapshot.restartPrompt)
        AskRestart();
}

void CUIModsWnd::RefreshRows()
{
    // what the rows show changes with the states alone
    u64 signature = 1469598103934665603ull;
    const auto mix = [&signature](u64 value) { signature = (signature ^ value) * 1099511628211ull; };
    for (const ModUpdateService::ModuleStatus& module : m_snapshot.modules)
        mix(static_cast<u64>(module.state));
    for (const ModUpdateService::ModuleStatus& install : m_snapshot.installs)
        mix(static_cast<u64>(install.state) + 97);
    mix(m_enabledNext.size());
    if (signature == m_snapshotSignature)
        return;
    m_snapshotSignature = signature;
    m_detailsFor = size_t(-1);

    CUIXml& xml = *m_xml;
    for (CUIModsRow* row : m_rows)
    {
        const Item& item = m_items[row->index];
        const ModUpdateService::ModuleStatus* status = StatusOf(item);
        const State state = status ? status->state : State::NoSource;
        pcstr badge = nullptr;
        pcstr color = "muted";
        if (item.Installed() && XMS::StagedAction(item.id.c_str()) == "remove")
            badge = "st_mods_badge_removal", color = "bad";
        else if (state == State::Available)
            badge = "st_mods_badge_update", color = "accent";
        else if (mods_in_flight(state))
            badge = "st_mods_badge_working", color = "accent";
        else if (state == State::Staged)
            badge = "st_mods_badge_restart", color = "accent";
        // an update refused because the author withdrew the module is no fault: the card says why
        else if (state == State::Failed || (state == State::Refused && !item.withdrawn))
            badge = state == State::Refused ? "st_mods_badge_refused" : "st_mods_badge_failed", color = "bad";
        else if (item.Installed() && XMS::Modules()[item.module].disabled)
            badge = "st_mods_badge_off", color = "muted";
        else if (item.entry && item.entry->vt.verdict == ModCatalog::Verdict::Block)
            badge = "st_mods_badge_blocked", color = "bad";
        else if (m_view == View::Browser && item.Installed())
            badge = "st_mods_badge_installed", color = "good";
        row->SetBadge(badge ? mods_text(badge).c_str() : "", mods_color(xml, "main:colors", color, m_mutedColor));
    }

    // until the player picks a row, the installed view follows the first module with news
    if (m_autoSelect && m_view == View::Installed)
        for (size_t index = 0; index != m_items.size(); ++index)
        {
            const ModUpdateService::ModuleStatus* status = StatusOf(m_items[index]);
            if (status && (status->state == State::Available || mods_in_flight(status->state)))
            {
                m_autoSelect = false;
                Select(index, true);
                break;
            }
        }
}

void CUIModsWnd::FillDetails(const Item& item)
{
    m_name->SetText(item.name.c_str());
    m_author->SetText(item.author.empty() ? "" : mods_format("st_mods_author", item.author.c_str()).c_str());
    xr_string meta = mods_format("st_mods_version", item.version.empty() ? "-" : item.version.c_str());
    if (!item.mode.empty())
        meta += " \xB7 " + item.mode;
    m_meta->SetText(meta.c_str());
    m_tagsLine->SetText(item.tagsText.c_str());

    const bool thumbnail = !item.thumbnail.empty();
    m_thumb->Show(thumbnail);
    m_initial->Show(!thumbnail);
    if (thumbnail)
    {
        m_thumb->InitTexture(item.thumbnail.c_str(), false);
        m_thumb->TextureOn();
    }
    else
        m_initial->SetText(mods_initial(item.name).c_str());

    const ModCatalog::Rating rating = item.entry ? item.entry->rating : ModCatalog::Rating{};
    const float average = rating.average100 / 100.f;
    for (u32 star = 0; star != 5; ++star)
        m_stars[star]->style.fraction = std::clamp(average - static_cast<float>(star), 0.f, 1.f);
    m_rating->SetText(!item.entry          ? mods_text("st_mods_rating_unlisted").c_str() :
            rating.count ? mods_format("st_mods_rating", average, rating.count).c_str() :
                           mods_text("st_mods_rating_none").c_str());

    m_body->Clear();
    CUIXml& xml = *m_xml;
    CUIStatic* description = xr_new<CUIStatic>("Mod description");
    CUIXmlInit::InitStatic(xml, "main:details:body_text", 0, description);
    description->SetWidth(m_body->GetDesiredChildWidth());
    description->SetText(item.description.empty() ? mods_text("st_mods_no_description").c_str() : item.description.c_str());
    description->AdjustHeightToText();
    m_body->AddWindow(description, true);
    m_reviewsShown.reset();
    BuildReviews(item);
    m_body->ScrollToBegin();
    if (item.entry)
        ModCatalog::WantReviews(item.id.c_str());
}

// The reviews under the description: a heading with "Write a review", then one block per review.
void CUIModsWnd::BuildReviews(const Item& item)
{
    CUIXml& xml = *m_xml;
    // everything after the description goes: the heading and the reviews of the last build
    while (m_body->GetSize() > 1)
        m_body->RemoveWindow(m_body->GetItem(m_body->GetSize() - 1));
    m_writeReview = nullptr;
    if (!item.entry)
        return;

    CUIWindow* heading = xr_new<CUIWindow>("Reviews heading");
    CUIXmlInit::InitWindow(xml, "main:details:reviews_head", 0, heading);
    heading->SetWidth(m_body->GetDesiredChildWidth());
    CUIStatic* title = UIHelper::CreateStatic(xml, "main:details:reviews_head:title", heading);
    title->SetText(mods_text("st_mods_reviews").c_str());
    m_writeReview = mods_button(xml, "main:details:reviews_head:write", heading);
    Fvector2 position = m_writeReview->GetWndPos();
    position.x += heading->GetWidth();
    m_writeReview->SetWndPos(position);
    Register(m_writeReview);
    AddCallback(m_writeReview, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIModsWnd::OnWriteReview));
    m_body->AddWindow(heading, true);

    const std::shared_ptr<const ModCatalog::Reviews> reviews = ModCatalog::GetReviews(item.id.c_str());
    m_reviewsShown = reviews;
    if (!reviews || reviews->status != ModCatalog::Status::Ready || reviews->list.empty())
    {
        CUIStatic* note = xr_new<CUIStatic>("Reviews note");
        CUIXmlInit::InitStatic(xml, "main:details:body_note", 0, note);
        note->SetWidth(m_body->GetDesiredChildWidth());
        pcstr text = !reviews || reviews->status == ModCatalog::Status::Loading || reviews->status == ModCatalog::Status::Idle ?
            "st_mods_reviews_loading" :
            reviews->status == ModCatalog::Status::Ready ? "st_mods_reviews_none" : "st_mods_reviews_failed";
        note->SetText(mods_text(text).c_str());
        m_body->AddWindow(note, true);
        return;
    }

    const float starStep = xml.ReadAttribFlt("main:details:review:star", 0, "step", 13.f);
    for (const ModCatalog::Review& review : reviews->list)
    {
        CUIWindow* block = xr_new<CUIWindow>("Review");
        CUIXmlInit::InitWindow(xml, "main:details:review", 0, block);
        block->SetWidth(m_body->GetDesiredChildWidth());
        for (u32 star = 0; star != 5; ++star)
        {
            CUIShape* shape = mods_shape(xml, "main:details:review:star", block);
            shape->style.fraction = star < review.stars ? 1.f : 0.f;
            Fvector2 place = shape->GetWndPos();
            place.x += starStep * star;
            shape->SetWndPos(place);
        }
        CUIStatic* head = UIHelper::CreateStatic(xml, "main:details:review:head", block);
        const xr_string name = review.name.empty() ?
            mods_format("st_mods_review_anonymous", review.keyId.substr(0, 6).c_str()) :
            mods_to_ui(review.name);
        head->SetText(mods_format("st_mods_review_head", name.c_str(), review.version.c_str(), review.date.c_str()).c_str());
        CUIStatic* text = UIHelper::CreateStatic(xml, "main:details:review:text", block);
        text->SetWidth(block->GetWidth() - text->GetWndPos().x);
        text->SetText(mods_multiline(mods_to_ui(review.text)).c_str());
        text->AdjustHeightToText();
        block->SetHeight(text->GetWndPos().y + text->GetHeight() + xml.ReadAttribFlt("main:details:review", 0, "bottom", 10.f));
        m_body->AddWindow(block, true);
    }
}

void CUIModsWnd::RefreshDetails()
{
    const Item* item = Selected();
    // the card's parts appear with a selection; which of them show is FillDetails' and below
    if ((item != nullptr) != m_detailsShown)
    {
        m_detailsShown = item != nullptr;
        for (CUIWindow* child : m_details->GetChildWndList())
            child->Show(m_detailsShown);
        m_details->GetChildWndList().front()->Show(true);
        m_detailsFor = size_t(-1);
    }
    m_detailsHint->Show(!item);
    if (!item)
        return;
    // shown again below by what is true of the selected module now
    m_progressTrack->Show(false);
    m_progressFill->Show(false);

    if (m_detailsFor != m_selected)
    {
        m_detailsFor = m_selected;
        FillDetails(*item);
    }
    // reviews arrive later than the card
    if (item->entry)
    {
        const std::shared_ptr<const ModCatalog::Reviews> reviews = ModCatalog::GetReviews(item->id.c_str());
        if (reviews != m_reviewsShown)
            BuildReviews(*item);
    }

    // the status line
    const ModUpdateService::ModuleStatus* status = StatusOf(*item);
    const State state = status ? status->state : State::NoSource;
    const bool installed = item->Installed();
    const xr_string staged = installed ? XMS::StagedAction(item->id.c_str()) : xr_string();
    auto [next, fresh] = m_enabledNext.try_emplace(item->id.c_str(), true);
    if (fresh && installed)
        next->second = XMS::ModuleEnabledNextStart(item->id.c_str());
    const bool disabledNow = installed && XMS::Modules()[item->module].disabled;
    const bool enabledNext = next->second;

    xr_string text;
    u32 color = m_mutedColor;
    switch (state)
    {
    case State::NoSource: text = mods_text(installed ? "st_mods_status_no_source" : "st_mods_status_not_installed").c_str(); break;
    case State::Unchecked:
    case State::Checking: text = mods_text("st_mods_status_checking").c_str(); break;
    case State::Current: text = mods_text("st_mods_status_current").c_str(), color = m_goodColor; break;
    case State::NoRelease: text = mods_text("st_mods_status_no_release").c_str(); break;
    case State::Available:
        text = mods_format(status->reissue ? "st_mods_status_reissue" : "st_mods_status_available", status->version.c_str(),
            mods_size(status->totalBytes).c_str());
        color = m_accentColor;
        break;
    case State::Blocked:
        text = status->requiresGame.empty() ? xr_string(mods_text("st_mods_status_blocked_schema").c_str()) :
                                              mods_format("st_mods_status_blocked", status->version.c_str(), status->requiresGame.c_str());
        color = m_badColor;
        break;
    case State::Refused: text = mods_format("st_mods_status_refused", status->message.c_str()), color = m_badColor; break;
    case State::CheckFailed: text = mods_text("st_mods_status_check_failed").c_str(), color = m_badColor; break;
    case State::Queued: text = mods_text("st_mods_status_queued").c_str(), color = m_accentColor; break;
    case State::Downloading:
        text = mods_format("st_mods_status_downloading", mods_size(status->downloadedBytes).c_str(), mods_size(status->totalBytes).c_str());
        color = m_accentColor;
        break;
    case State::Preparing:
        text = mods_format("st_mods_status_preparing", mods_size(status->downloadedBytes).c_str(), mods_size(status->totalBytes).c_str());
        color = m_accentColor;
        break;
    case State::Staged:
        text = mods_format(installed ? "st_mods_status_staged" : "st_mods_status_install_staged", status->version.c_str());
        color = m_accentColor;
        break;
    case State::Failed: text = mods_format("st_mods_status_failed", status->message.c_str()), color = m_badColor; break;
    }
    // installed by hand and not byte for byte a release of the browser: "up to date" would claim too much
    if (installed && status && status->handModified && state == State::Current)
        text = mods_text("st_mods_status_local_modified").c_str(), color = m_warnColor;
    // what an update would say matters no more; one already on its way still does
    if (item->withdrawn && !mods_in_flight(state) && state != State::Staged)
        text = mods_text("st_mods_status_author_withdrawn").c_str(), color = m_warnColor;
    if (staged == "remove")
        text = mods_text("st_mods_status_removal").c_str(), color = m_badColor;
    else if (installed && disabledNow != !enabledNext)
        text = mods_text(enabledNext ? "st_mods_status_enable_next" : "st_mods_status_disable_next").c_str(), color = m_accentColor;
    else if (disabledNow)
        text = xr_string(mods_text("st_mods_status_disabled").c_str()) + ". " + text;
    // a revocation outranks everything the line could say, a withdrawn module's too
    const xr_string& revoked = item->entry ? item->entry->revoked : item->revoked;
    if (!revoked.empty())
        text = mods_format("st_mods_status_withdrawn", revoked.c_str()), color = m_badColor;
    if (0 != xr_strcmp(m_status->GetText(), text.c_str()))
        m_status->SetText(text.c_str());
    m_status->SetTextColor(color);

    const bool downloading = state == State::Downloading || state == State::Preparing;
    m_progressTrack->Show(downloading);
    m_progressFill->Show(downloading);
    if (downloading)
    {
        const float progress = status->totalBytes ? static_cast<float>(status->downloadedBytes) / static_cast<float>(status->totalBytes) : 0.f;
        m_progressFill->SetWidth(std::max(1.f, m_progressWidth * std::clamp(progress, 0.f, 1.f)));
    }

    // VirusTotal: what the catalog or the player's own lookup says
    const bool listed = item->entry != nullptr;
    m_vt->Show(listed);
    m_vtCheck->Show(listed);
    m_vtReport->Show(listed);
    if (listed)
    {
        const ModCatalog::VirusTotal& vt = item->entry->vt;
        xr_string line;
        u32 vtColor = m_mutedColor;
        if (ModCatalog::VirusTotalBusy(item->id.c_str()))
            line = mods_text("st_mods_vt_checking").c_str();
        else if (vt.verdict == ModCatalog::Verdict::Clean)
            line = mods_format(vt.live ? "st_mods_vt_clean_live" : "st_mods_vt_clean", vt.date.c_str()), vtColor = m_goodColor;
        else if (vt.verdict == ModCatalog::Verdict::Warn)
            line = mods_format("st_mods_vt_warn", vt.flaggedBy.c_str()), vtColor = m_warnColor;
        else if (vt.verdict == ModCatalog::Verdict::Block)
            line = mods_format("st_mods_vt_block", vt.flaggedBy.c_str()), vtColor = m_badColor;
        else
            line = mods_text(vt.unknown ? "st_mods_vt_unseen" : "st_mods_vt_unknown").c_str();
        if (0 != xr_strcmp(m_vt->GetText(), line.c_str()))
            m_vt->SetText(line.c_str());
        m_vt->SetTextColor(vtColor);
        m_vtCheck->Enable(!ModCatalog::VirusTotalBusy(item->id.c_str()));
    }

    // the buttons
    const bool blocked = listed && (item->entry->vt.verdict == ModCatalog::Verdict::Block || !item->entry->revoked.empty());
    pcstr primary = "st_mods_update";
    bool primaryEnabled = false;
    if (staged == "remove")
        primary = "st_mods_keep", primaryEnabled = true;
    else if (installed)
    {
        primaryEnabled = state == State::Available || state == State::Failed;
        primary = state == State::Failed ? "st_mods_retry" : "st_mods_update";
    }
    else
    {
        primary = state == State::Failed || state == State::Refused ? "st_mods_retry" : "st_mods_install";
        primaryEnabled = !blocked && (!status || state == State::Failed || state == State::Refused);
    }
    if (0 != xr_strcmp(m_primary->GetText(), mods_text(primary).c_str()))
        m_primary->SetText(mods_text(primary).c_str());
    m_primary->Enable(primaryEnabled);
    m_website->Enable(!item->website.empty());
    m_toggle->Show(installed);
    m_remove->Show(installed);
    if (installed)
    {
        pcstr toggle = enabledNext ? "st_mods_disable" : "st_mods_enable";
        if (0 != xr_strcmp(m_toggle->GetText(), mods_text(toggle).c_str()))
            m_toggle->SetText(mods_text(toggle).c_str());
        m_toggle->Enable(staged != "remove");
        m_remove->Enable(staged != "remove" && !mods_in_flight(state));
    }
    if (m_writeReview)
        m_writeReview->Enable(installed && m_catalog->reviewsWritable &&
            ModCatalog::GetPostStatus().state != ModCatalog::PostState::Working &&
            ModCatalog::GetPostStatus().state != ModCatalog::PostState::Identity &&
            ModCatalog::GetPostStatus().state != ModCatalog::PostState::Sending);
}

void CUIModsWnd::SendMessage(CUIWindow* window, s16 message, void* data)
{
    if (message == LIST_ITEM_CLICKED && data)
        for (CUIModsRow* row : m_rows)
            if (row == window)
            {
                m_autoSelect = false;
                Select(*static_cast<size_t*>(data), false);
            }
    CUIWndCallback::OnEvent(window, message, data);
    inherited::SendMessage(window, message, data);
}

bool CUIModsWnd::OnKeyboardAction(int dik, EUIMessages keyboardAction)
{
    if (keyboardAction == WINDOW_KEY_PRESSED)
    {
        if (IsBinded(kQUIT, dik))
        {
            HideDialog();
            return true;
        }
        switch (GetBindedAction(dik, EKeyContext::UI))
        {
        case kUI_MOVE_UP: MoveSelection(-1); return true;
        case kUI_MOVE_DOWN: MoveSelection(1); return true;
        default: break;
        }
    }
    return inherited::OnKeyboardAction(dik, keyboardAction);
}

// ---- questions --------------------------------------------------------------------------------------

// What VirusTotal said about a package the player is about to download, when it was a warning.
static xr_string mods_vt_warning(const ModCatalog::Entry* entry)
{
    if (!entry || entry->vt.verdict != ModCatalog::Verdict::Warn)
        return {};
    return mods_format("st_mods_vt_warn_prompt", entry->vt.flaggedBy.c_str()) + "\\n \\n";
}

void CUIModsWnd::AskInstall(const Item& item)
{
    CUIModsPrompt::Question question;
    question.caption = mods_format("st_mods_install_caption", item.name.c_str());
    const u64 bytes = item.entry ? item.entry->release.PackageBytes() : 0;
    question.text = mods_vt_warning(item.entry) + mods_format("st_mods_install_text", item.version.c_str(), mods_size(bytes).c_str()) +
        "\\n \\n" + mods_text("st_mods_disclaimer").c_str();
    question.accept = mods_text("st_mods_install").c_str();
    // the answer acts on what is selected then, which is this item: the prompt is modal
    question.onAccept = CUIModsPrompt::Callback(this, &CUIModsWnd::OnPrimary);
    m_prompt->Ask(std::move(question));
    m_prompt->SetWindowName(("install:" + item.id).c_str());
}

void CUIModsWnd::AskUpdate(const Item& item)
{
    const ModUpdateService::ModuleStatus* status = StatusOf(item);
    CUIModsPrompt::Question question;
    question.caption = mods_format("st_mods_update_caption", item.name.c_str());
    question.text = mods_vt_warning(item.entry) +
        mods_format("st_mods_update_text", status ? status->version.c_str() : "", mods_size(status ? status->totalBytes : 0).c_str()) +
        "\\n \\n" + mods_text("st_mods_disclaimer").c_str();
    question.accept = mods_text("st_mods_update").c_str();
    question.onAccept = CUIModsPrompt::Callback(this, &CUIModsWnd::OnPrimary);
    m_prompt->Ask(std::move(question));
    m_prompt->SetWindowName(("update:" + item.id).c_str());
}

void CUIModsWnd::AskUpdateAll()
{
    u32 available = 0;
    for (const ModUpdateService::ModuleStatus& module : m_snapshot.modules)
        available += module.state == State::Available;
    if (!available)
        return;
    CUIModsPrompt::Question question;
    question.caption = mods_format("st_mods_update_all_caption", available);
    question.text = mods_text("st_mods_disclaimer").c_str();
    question.accept = mods_text("st_mods_update_all").c_str();
    question.onAccept = CUIModsPrompt::Callback(this, &CUIModsWnd::OnUpdateAll);
    m_prompt->Ask(std::move(question));
    m_prompt->SetWindowName("update_all");
}

void CUIModsWnd::AskRemove(const Item& item)
{
    CUIModsPrompt::Question question;
    question.caption = mods_format("st_mods_remove_caption", item.name.c_str());
    question.text = mods_format("st_mods_remove_text", item.id.c_str());
    question.accept = mods_text("st_mods_remove").c_str();
    question.danger = true;
    question.onAccept = CUIModsPrompt::Callback(this, &CUIModsWnd::OnRemove);
    m_prompt->Ask(std::move(question));
    m_prompt->SetWindowName(("remove:" + item.id).c_str());
}

void CUIModsWnd::AskProtection()
{
    CUIModsPrompt::Question question;
    const bool on = XMS::Catalog::ProtectionLatched();
    if (on && XMS::Catalog::CatalogModuleEnabled())
    {
        question.caption = mods_text("st_mods_protect_caption").c_str();
        question.text = mods_text("st_mods_protect_locked").c_str();
    }
    else
    {
        question.caption = mods_text(on ? "st_mods_protect_off_caption" : "st_mods_protect_on_caption").c_str();
        question.text = mods_text(on ? "st_mods_protect_off_text" : "st_mods_protect_on_text").c_str();
        question.accept = mods_text(on ? "st_mods_turn_off" : "st_mods_turn_on").c_str();
        question.danger = on;
        question.enterAccepts = !on;
        question.onAccept = CUIModsPrompt::Callback(this, &CUIModsWnd::OnProtect);
    }
    m_prompt->Ask(std::move(question));
    m_prompt->SetWindowName(on ? "protect_off" : "protect_on");
}

void CUIModsWnd::AskVirusTotalKey(const xr_string& id)
{
    CUIModsPrompt::Question question;
    question.caption = mods_text("st_mods_vt_key_caption").c_str();
    question.text = mods_text("st_mods_vt_key_text").c_str();
    question.accept = mods_text("st_mods_save").c_str();
    question.enterAccepts = true;
    question.input = true;
    question.onAccept = CUIModsPrompt::Callback(this, &CUIModsWnd::OnVtCheck);
    m_prompt->Ask(std::move(question));
    m_prompt->SetWindowName(("vt_key:" + id).c_str());
}

void CUIModsWnd::AskRestart()
{
    // only from this window and only when it has the input: a box opened under another
    // dialog would answer to nobody
    if (!IsShown() || GetHolder()->TopInputReceiver() != this)
        return;
    ModUpdateService::AcknowledgeRestartPrompt();
    CUIModsPrompt::Question question;
    question.caption = mods_text("st_mods_restart_caption").c_str();
    question.text = mods_text("st_mods_restart_prompt").c_str();
    question.accept = mods_text("st_mods_restart").c_str();
    question.cancel = mods_text("st_mods_later").c_str();
    question.onAccept = CUIModsPrompt::Callback(this, &CUIModsWnd::OnRestart);
    m_prompt->Ask(std::move(question));
    m_prompt->SetWindowName("restart");
}

void CUIModsWnd::OpenReview(const Item& item)
{
    if (!item.entry || !item.Installed() || !m_catalog->reviewsWritable)
        return;
    m_review->Open(item.id.c_str(), item.name.c_str());
}

void CUIModsWnd::OnReviewSent()
{
    CUIModsPrompt::Question question;
    question.caption = mods_text("st_mods_review_sent_caption").c_str();
    question.text = mods_text("st_mods_review_sent").c_str();
    m_prompt->Ask(std::move(question));
    m_prompt->SetWindowName("review_sent");
}

// ---- buttons ------------------------------------------------------------------------------------------

void CUIModsWnd::OnInstalledTab(CUIWindow*, void*) { SetView(View::Installed); }
void CUIModsWnd::OnBrowserTab(CUIWindow*, void*) { SetView(View::Browser); }

void CUIModsWnd::OnProtect(CUIWindow* window, void*)
{
    // the chip asks; the prompt's answer (no window) acts
    if (window)
    {
        AskProtection();
        return;
    }
    xr_string error;
    if (!XMS::Catalog::SetProtectionLatched(!XMS::Catalog::ProtectionLatched(), error))
        Msg("! [mods] protected mode not switched: %s", error.c_str());
}

void CUIModsWnd::OnSort(CUIWindow*, void*)
{
    m_sort = static_cast<Sort>((static_cast<u8>(m_sort) + 1) % static_cast<u8>(Sort::Count));
    m_itemsDirty = true;
}

void CUIModsWnd::OnTag(CUIWindow* window, void*)
{
    if (!window)
        return;
    const xr_string tag = window->WindowName().c_str();
    if (const auto found = std::ranges::find(m_activeTags, tag); found != m_activeTags.end())
        m_activeTags.erase(found);
    else
        m_activeTags.push_back(tag);
    m_itemsDirty = true;
}

void CUIModsWnd::OnPrimary(CUIWindow* window, void*)
{
    const Item* item = Selected();
    if (!item)
        return;
    const ModUpdateService::ModuleStatus* status = StatusOf(*item);
    const State state = status ? status->state : State::NoSource;
    if (item->Installed() && XMS::StagedAction(item->id.c_str()) == "remove")
    {
        ModUpdateService::CancelRemoval(item->id.c_str());
        m_snapshotSignature = 0;
        return;
    }
    // every download is asked for with the disclaimer first (MOD_CATALOG 14)
    if (window)
    {
        if (item->Installed())
            AskUpdate(*item);
        else
            AskInstall(*item);
        return;
    }
    if (item->Installed())
    {
        if (state == State::Available || state == State::Failed)
            ModUpdateService::StartUpdate(item->id.c_str());
        return;
    }
    xr_string error;
    if (!ModUpdateService::StartInstall(item->id.c_str(), error))
    {
        CUIModsPrompt::Question question;
        question.caption = mods_format("st_mods_install_caption", item->name.c_str());
        question.text = mods_format("st_mods_install_refused", error.c_str());
        m_prompt->Ask(std::move(question));
        m_prompt->SetWindowName("install_refused");
    }
}

void CUIModsWnd::OnWebsite(CUIWindow*, void*)
{
    if (const Item* item = Selected())
    {
        if (item->Installed())
            ModUpdateService::OpenWebsite(item->id.c_str());
        else
            ModCatalog::OpenWebsite(item->id.c_str());
    }
}

void CUIModsWnd::OnToggle(CUIWindow*, void*)
{
    const Item* item = Selected();
    if (!item || !item->Installed())
        return;
    xr_string error;
    const bool enable = !XMS::ModuleEnabledNextStart(item->id.c_str());
    if (!XMS::SetModuleEnabled(item->id.c_str(), enable, error))
        Msg("! [mods] %s could not be switched: %s", item->id.c_str(), error.c_str());
    m_enabledNext.clear();
    m_snapshotSignature = 0;
}

void CUIModsWnd::OnRemove(CUIWindow* window, void*)
{
    const Item* item = Selected();
    if (!item || !item->Installed())
        return;
    if (window)
    {
        AskRemove(*item);
        return;
    }
    xr_string error;
    if (!ModUpdateService::StageRemoval(item->id.c_str(), error))
        Msg("! [mods] %s cannot be removed: %s", item->id.c_str(), error.c_str());
    m_snapshotSignature = 0;
}

void CUIModsWnd::OnVtCheck(CUIWindow* window, void*)
{
    const Item* item = Selected();
    if (!item || !item->entry)
        return;
    // the key prompt answers with no window: store what was typed, then look up
    if (!window && m_prompt->Input() && *m_prompt->Input())
    {
        xr_string error;
        if (!ModCatalog::SetVirusTotalKey(m_prompt->Input(), error))
        {
            CUIModsPrompt::Question question;
            question.caption = mods_text("st_mods_vt_key_caption").c_str();
            question.text = mods_format("st_mods_vt_key_refused", error.c_str());
            m_prompt->Ask(std::move(question));
            return;
        }
    }
    if (!ModCatalog::HasVirusTotalKey())
    {
        AskVirusTotalKey(item->id);
        return;
    }
    ModCatalog::CheckVirusTotal(item->id.c_str());
}

void CUIModsWnd::OnVtReport(CUIWindow*, void*)
{
    if (const Item* item = Selected())
        ModCatalog::OpenVirusTotalReport(item->id.c_str());
}

void CUIModsWnd::OnWriteReview(CUIWindow*, void*)
{
    if (const Item* item = Selected())
        OpenReview(*item);
}

void CUIModsWnd::OnUpdateAll(CUIWindow* window, void*)
{
    if (window)
        AskUpdateAll();
    else
        ModUpdateService::StartUpdateAll();
}

void CUIModsWnd::OnBack(CUIWindow*, void*) { HideDialog(); }

void CUIModsWnd::OnRestart(CUIWindow*, void*)
{
    if (!ModUpdateService::RestartGame())
        Msg("! [mods] could not restart the game - the updates are applied by the next start");
}

// ---- the console ------------------------------------------------------------------------------------

void CUIModsWnd::RunCommand(const xr_string& command)
{
    const size_t space = command.find(' ');
    const xr_string verb = command.substr(0, space);
    const xr_string rest = space == xr_string::npos ? xr_string() : command.substr(space + 1);
    if (verb == "view")
        SetView(rest == "browser" ? View::Browser : View::Installed);
    else if (verb == "search" || verb == "clear")
    {
        m_search->SetText(verb == "clear" ? "" : rest.c_str());
        m_query = m_search->GetText();
        m_itemsDirty = true;
    }
    else if (verb == "sort")
    {
        static constexpr pcstr names[] = {"relevance", "rating", "newest", "name"};
        for (u8 sort = 0; sort != static_cast<u8>(Sort::Count); ++sort)
            if (rest == names[sort])
                m_sort = static_cast<Sort>(sort);
        m_itemsDirty = true;
    }
    else if (verb == "tag")
    {
        for (CUIShapeButton* chip : m_tags)
            if (rest == chip->WindowName().c_str())
                OnTag(chip, nullptr);
    }
    else if (verb == "select")
    {
        if (m_itemsDirty)
            BuildItems();
        m_autoSelect = false;
        SelectId(rest);
    }
    else if (verb == "press")
    {
        // what a click sees: the card of what is selected now
        if (m_itemsDirty)
            BuildItems();
        m_snapshot = ModUpdateService::GetSnapshot();
        RefreshRows();
        RefreshDetails();
        const std::pair<pcstr, CUIShapeButton*> buttons[] = {{"primary", m_primary}, {"website", m_website},
            {"toggle", m_toggle}, {"remove", m_remove}, {"vt_check", m_vtCheck}, {"vt_report", m_vtReport},
            {"review", m_writeReview}, {"update_all", m_updateAll}, {"protect", m_protect}, {"sort", m_sortButton},
            {"installed", m_tabInstalled}, {"browser", m_tabBrowser}, {"back", m_back}};
        for (const auto& [name, button] : buttons)
            if (rest == name && button && button->IsEnabled() && button->IsShown())
                button->OnClick();
    }
    else if (verb == "answer" && m_prompt->IsShown())
        m_prompt->Answer(rest == "yes");
    else if (verb == "key" && m_prompt->IsShown())
        m_prompt->SetInput(rest.c_str());
    else if (verb == "review" && m_review->IsShown())
    {
        const size_t split = rest.find(' ');
        m_review->Fill(static_cast<u32>(atoi(rest.substr(0, split).c_str())), split == xr_string::npos ? "" : rest.c_str() + split + 1);
    }
    else if (verb == "send" && m_review->IsShown())
        m_review->Send();
    else if (verb == "state")
        LogState();
    else if (verb == "wait")
        m_queueResume = Device.dwTimeGlobal + static_cast<u32>(std::max(0, atoi(rest.c_str())));
    else if (verb == "mark")
        Msg("* [mods-ui] mark %s", rest.c_str());
    else
        Msg("* [mods-ui] commands: view installed|browser, search <words>, clear, sort relevance|rating|newest|name, "
            "tag <tag>, select <id>, press primary|website|toggle|remove|vt_check|vt_report|review|update_all|protect|sort|"
            "installed|browser|back, answer yes|no, key <text>, review <stars> <text>, send, state, wait <ms>, mark <text>");
}

void CUIModsWnd::LogState()
{
    if (m_itemsDirty)
        BuildItems();
    static constexpr pcstr sorts[] = {"relevance", "rating", "newest", "name"};
    xr_string tags;
    for (const xr_string& tag : m_activeTags)
        tags += (tags.empty() ? "" : ",") + tag;
    Msg("* [mods-ui] view %s, %zu item(s), sort %s, query \"%s\", tags %s, catalog %u, list \"%s\"",
        m_view == View::Browser ? "browser" : "installed", m_items.size(), sorts[static_cast<size_t>(m_sort)], m_query.c_str(),
        tags.empty() ? "-" : tags.c_str(), static_cast<u32>(m_catalog->status), m_listStatus->IsShown() ? m_listStatus->GetText() : "");
    for (const CUIModsRow* row : m_rows)
    {
        const Item& item = m_items[row->index];
        Msg("* [mods-ui] row %s \"%s\" badge=%s rating=%s thumbnail=%s%s", item.id.c_str(), item.name.c_str(),
            row->badgeShown.empty() ? "-" : row->badgeShown.c_str(), row->rating->IsShown() ? row->rating->GetText() : "-",
            row->thumb->IsShown() ? "yes" : "no", row->selected ? " selected" : "");
    }
    if (const Item* item = Selected())
        Msg("* [mods-ui] card %s: status \"%s\", primary \"%s\"%s, vt \"%s\", rating \"%s\", reviews %u, review button %s",
            item->id.c_str(), m_status->GetText(), m_primary->GetText(), m_primary->IsEnabled() ? "" : " (off)",
            m_vt->IsShown() ? m_vt->GetText() : "-", m_rating->GetText(), m_body->GetSize() > 2 ? m_body->GetSize() - 2 : 0,
            m_writeReview ? (m_writeReview->IsEnabled() ? "on" : "off") : "none");
    if (m_prompt->IsShown())
        Msg("* [mods-ui] prompt %s \"%s\"", m_prompt->WindowName().c_str(), m_prompt->Caption().c_str());
    if (m_review->IsShown())
        Msg("* [mods-ui] review dialog for %s", m_review->ModuleId().c_str());
}
