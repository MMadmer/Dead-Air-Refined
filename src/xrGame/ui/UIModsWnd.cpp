#include "StdAfx.h"
#include "UIModsWnd.h"

#include "UIDialogHolder.h"
#include "UIHelper.h"
#include "UIMessageBoxEx.h"
#include "UIXmlInit.h"
#include "xrCore/XMS/xms_core.h"
#include "xrEngine/xr_input.h"
#include "xrEngine/xr_level_controller.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/ListBox/UIListBox.h"
#include "xrUICore/ListBox/UIListBoxItem.h"
#include "xrUICore/ProgressBar/UIProgressBar.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Static/UIStatic.h"

namespace
{
using ModUpdateService::State;

// by value: an id the table does not know comes back as a temporary
shared_str mods_text(pcstr id) { return StringTable().translate(id); }

// "3.2 MB", or kilobytes for a release small enough to read as "0.0 MB"
xr_string mods_size(u64 bytes)
{
    string64 text{};
    if (bytes < 1024 * 1024)
        xr_sprintf(text, sizeof(text), mods_text("st_mods_size_kb").c_str(), static_cast<float>(bytes) / 1024.f);
    else
        xr_sprintf(text, sizeof(text), mods_text("st_mods_size_mb").c_str(),
            static_cast<float>(bytes) / (1024.f * 1024.f));
    return text;
}

// The UI draws one byte per character in Windows-1251, and a manifest is UTF-8 unless somebody
// wrote it by hand in the game's own code page - in which case it is already what the UI wants.
xr_string mods_to_ui(const xr_string& text)
{
#ifdef XR_PLATFORM_WINDOWS
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
#else
    return text;
#endif
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
}

CUIModsWnd::CUIModsWnd() : CUIDialogWnd(CUIModsWnd::GetDebugType())
{
    m_bWorkInPause = true;
}

CUIModsWnd::~CUIModsWnd() { xr_delete(m_restartBox); }

bool CUIModsWnd::Init()
{
    CUIXml xml;
    if (!xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_mods.xml", false))
        return false;

    CUIXmlInit::InitWindow(xml, "main", 0, this);
    UIHelper::CreateStatic(xml, "main:background", this);
    UIHelper::CreateStatic(xml, "main:backdrop", this, false);
    UIHelper::CreateStatic(xml, "main:caption", this);
    UIHelper::CreateStatic(xml, "main:list_back", this, false);
    UIHelper::CreateStatic(xml, "main:details_back", this, false);
    UIHelper::CreateStatic(xml, "main:column_name", this);
    UIHelper::CreateStatic(xml, "main:column_author", this);
    UIHelper::CreateStatic(xml, "main:column_version", this);
    m_list = UIHelper::CreateListBox(xml, "main:list", this);
    m_empty = UIHelper::CreateStatic(xml, "main:empty", this);
    m_name = UIHelper::CreateStatic(xml, "main:name", this);
    m_author = UIHelper::CreateStatic(xml, "main:author", this);
    m_version = UIHelper::CreateStatic(xml, "main:version", this);
    m_status = UIHelper::CreateStatic(xml, "main:status", this);
    m_progress = UIHelper::CreateProgressBar(xml, "main:progress", this);
    m_description = UIHelper::CreateScrollView(xml, "main:description", this);
    m_descriptionText = xr_new<CUIStatic>("Mod description");
    CUIXmlInit::InitStatic(xml, "main:description_text", 0, m_descriptionText);
    m_descriptionText->SetWidth(m_description->GetDesiredChildWidth());
    m_description->AddWindow(m_descriptionText, true);
    m_update = UIHelper::Create3tButton(xml, "main:update", this);
    m_updateAll = UIHelper::Create3tButton(xml, "main:update_all", this);
    m_website = UIHelper::Create3tButton(xml, "main:website", this);
    m_back = UIHelper::Create3tButton(xml, "main:back", this);

    // the three columns of a row line up with the headers above the list
    m_nameWidth = xml.ReadAttribFlt("main:list", 0, "name_width", 280.f);
    m_authorWidth = xml.ReadAttribFlt("main:list", 0, "author_width", 140.f);
    m_versionWidth = xml.ReadAttribFlt("main:list", 0, "version_width", 70.f);
    m_normalColor = m_list->GetTextColor();
    m_disabledColor = CUIXmlInit::GetColor(xml, "main:list:color_disabled", 0, 0xff808080);
    m_updateColor = CUIXmlInit::GetColor(xml, "main:list:color_update", 0, 0xffd2be73);
    m_statusColor = m_status->GetTextColor();
    m_errorColor = CUIXmlInit::GetColor(xml, "main:status:color_error", 0, 0xffdc645a);

    Register(m_update);
    Register(m_updateAll);
    Register(m_website);
    Register(m_back);
    AddCallback(m_update, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIModsWnd::OnUpdate));
    AddCallback(m_updateAll, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIModsWnd::OnUpdateAll));
    AddCallback(m_website, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIModsWnd::OnWebsite));
    AddCallback(m_back, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIModsWnd::OnBack));

    // The stock yes/no box. Without it an update still lands on the next start - the status
    // line says so - the player is just not offered the shortcut.
    m_restartBox = xr_new<CUIMessageBoxEx>();
    if (m_restartBox->InitMessageBox("message_box_yes_no"))
        m_restartBox->func_on_ok = CUIWndCallback::void_function(this, &CUIModsWnd::OnRestartYes);
    else
        xr_delete(m_restartBox);

    BuildRows();
    return true;
}

void CUIModsWnd::BuildRows()
{
    for (const XMS::Module& module : XMS::Modules())
    {
        Row& row = m_rows.emplace_back();
        row.id = module.id;
        row.name = mods_display(module.name);
        row.author = mods_display(module.author);
        row.description = mods_multiline(mods_display(module.description));
        row.version = module.version;
        row.disabled = module.disabled;
        row.website = !module.website.empty();

        row.item = m_list->AddItem();
        row.item->SetTAG(static_cast<u32>(m_rows.size() - 1));
        row.item->SetTextColor(row.disabled ? m_disabledColor : m_normalColor);
        row.item->SetText(row.name.c_str());
        row.item->GetTextItem()->SetWidth(m_nameWidth);
        row.item->GetTextItem()->SetEllipsis(true);
        CUIStatic* author = row.item->AddTextField(row.author.c_str(), m_authorWidth);
        author->SetEllipsis(true);
        row.item->AddTextField(row.version.c_str(), m_versionWidth);
        // fields are placed edge to edge; narrowing them afterwards is what leaves a gutter
        // between a clipped name and the column next to it
        constexpr float gutter = 8.f;
        row.item->GetTextItem()->SetWidth(m_nameWidth - gutter);
        author->SetWidth(m_authorWidth - gutter);
    }
    m_empty->Show(m_rows.empty());
}

void CUIModsWnd::Show(bool status)
{
    inherited::Show(status);
    if (!status)
        return;
    // a check that failed - no network at startup, say - gets another go whenever the player looks
    ModUpdateService::StartCheck();
    m_detailsFor = size_t(-1);
    m_autoSelect = true;
    if (m_selected >= m_rows.size() && !m_rows.empty())
        SelectRow(0);
    Refresh(ModUpdateService::GetSnapshot());
}

void CUIModsWnd::Update()
{
    inherited::Update();
    const ModUpdateService::Snapshot snapshot = ModUpdateService::GetSnapshot();
    Refresh(snapshot);
    if (snapshot.restartPrompt)
        AskForRestart();
}

void CUIModsWnd::SendMessage(CUIWindow* window, s16 message, void* data)
{
    if (window == m_list && message == LIST_ITEM_SELECT && data)
    {
        m_autoSelect = false;
        SelectRow(*static_cast<u32*>(data));
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
        // The list follows the same keys as the main menu it was opened from. The buttons take
        // their accelerators from the layout.
        switch (GetBindedAction(dik, EKeyContext::UI))
        {
        case kUI_MOVE_UP: MoveSelection(-1); return true;
        case kUI_MOVE_DOWN: MoveSelection(1); return true;
        default: break;
        }
    }
    return inherited::OnKeyboardAction(dik, keyboardAction);
}

void CUIModsWnd::MoveSelection(int step)
{
    if (m_rows.empty())
        return;
    m_autoSelect = false;
    const int last = static_cast<int>(m_rows.size()) - 1;
    const int current = m_selected < m_rows.size() ? static_cast<int>(m_selected) : (step > 0 ? -1 : last + 1);
    SelectRow(static_cast<size_t>(std::clamp(current + step, 0, last)));
    m_list->ScrollToWindow(m_rows[m_selected].item);
}

void CUIModsWnd::SelectRow(size_t index)
{
    if (index >= m_rows.size())
        return;
    m_selected = index;
    m_list->SetSelected(m_rows[index].item);
}

void CUIModsWnd::Refresh(const ModUpdateService::Snapshot& snapshot)
{
    bool anyAvailable = false;
    for (size_t index = 0; index != m_rows.size() && index != snapshot.modules.size(); ++index)
    {
        Row& row = m_rows[index];
        const State state = snapshot.modules[index].state;
        anyAvailable |= state == State::Available;
        if (state == row.shownState)
            continue;
        row.shownState = state;
        // a row with something to take, or something taken, stands out in the list
        const bool marked = state == State::Available || state == State::Staged || mods_in_flight(state);
        row.item->SetTextColor(marked ? m_updateColor : row.disabled ? m_disabledColor : m_normalColor);
        for (CUIWindow* field : row.item->GetChildWndList())
        {
            if (auto* text = smart_cast<CUIStatic*>(field))
                text->SetTextColor(row.item->GetTextColor());
        }
        m_detailsFor = size_t(-1);
    }
    m_updateAll->Enable(anyAvailable);

    // The reason the window was opened is usually an update, and the check that finds it may
    // still be running when the window appears.
    if (m_autoSelect)
    {
        for (size_t index = 0; index != m_rows.size() && index != snapshot.modules.size(); ++index)
        {
            const State state = snapshot.modules[index].state;
            if (state != State::Available && !mods_in_flight(state))
                continue;
            m_autoSelect = false;
            SelectRow(index);
            m_list->ScrollToWindow(m_rows[index].item);
            break;
        }
    }

    const bool selected = m_selected < m_rows.size() && m_selected < snapshot.modules.size();
    m_name->Show(selected);
    m_author->Show(selected);
    m_version->Show(selected);
    m_status->Show(selected);
    m_description->Show(selected);
    if (selected)
        RefreshDetails(m_rows[m_selected], snapshot.modules[m_selected]);
    else
    {
        m_progress->Show(false);
        m_update->Enable(false);
        m_website->Enable(false);
    }
}

void CUIModsWnd::RefreshDetails(const Row& row, const ModUpdateService::ModuleStatus& status)
{
    if (m_detailsFor != m_selected)
    {
        m_detailsFor = m_selected;
        string256 line{};
        m_name->SetText(row.name.c_str());
        xr_sprintf(line, sizeof(line), mods_text("st_mods_author").c_str(), row.author.empty() ? "-" : row.author.c_str());
        m_author->SetText(line);
        xr_sprintf(line, sizeof(line), mods_text("st_mods_version").c_str(), row.version.empty() ? "-" : row.version.c_str());
        m_version->SetText(line);
        m_descriptionText->SetText(row.description.empty() ? mods_text("st_mods_no_description").c_str() : row.description.c_str());
        m_descriptionText->AdjustHeightToText();
        m_description->ScrollToBegin();
        m_website->Enable(row.website);
    }

    string512 text{};
    switch (status.state)
    {
    case State::NoSource: xr_strcpy(text, mods_text("st_mods_status_no_source").c_str()); break;
    case State::Unchecked:
    case State::Checking: xr_strcpy(text, mods_text("st_mods_status_checking").c_str()); break;
    case State::Current: xr_strcpy(text, mods_text("st_mods_status_current").c_str()); break;
    case State::NoRelease: xr_strcpy(text, mods_text("st_mods_status_no_release").c_str()); break;
    case State::Available:
        xr_sprintf(text, sizeof(text), mods_text("st_mods_status_available").c_str(), status.version.c_str(),
            mods_size(status.totalBytes).c_str());
        break;
    case State::Blocked:
        if (status.requiresGame.empty())
            xr_strcpy(text, mods_text("st_mods_status_blocked_schema").c_str());
        else
            xr_sprintf(text, sizeof(text), mods_text("st_mods_status_blocked").c_str(), status.version.c_str(),
                status.requiresGame.c_str());
        break;
    case State::CheckFailed: xr_strcpy(text, mods_text("st_mods_status_check_failed").c_str()); break;
    case State::Queued: xr_strcpy(text, mods_text("st_mods_status_queued").c_str()); break;
    case State::Downloading:
        xr_sprintf(text, sizeof(text), mods_text("st_mods_status_downloading").c_str(),
            mods_size(status.downloadedBytes).c_str(), mods_size(status.totalBytes).c_str());
        break;
    case State::Preparing:
        xr_sprintf(text, sizeof(text), mods_text("st_mods_status_preparing").c_str(),
            mods_size(status.downloadedBytes).c_str(), mods_size(status.totalBytes).c_str());
        break;
    case State::Staged:
        xr_sprintf(text, sizeof(text), mods_text("st_mods_status_staged").c_str(), status.version.c_str());
        break;
    case State::Failed: xr_strcpy(text, mods_text("st_mods_status_failed").c_str()); break;
    }
    if (row.disabled)
    {
        string512 prefixed{};
        xr_sprintf(prefixed, sizeof(prefixed), "%s. %s", mods_text("st_mods_status_disabled").c_str(), text);
        xr_strcpy(text, prefixed);
    }
    if (0 != xr_strcmp(m_status->GetText(), text))
        m_status->SetText(text);
    const bool error = status.state == State::CheckFailed || status.state == State::Failed ||
        status.state == State::Blocked;
    m_status->SetTextColor(error ? m_errorColor : m_statusColor);

    const bool downloading = status.state == State::Downloading || status.state == State::Preparing;
    m_progress->Show(downloading);
    if (downloading)
    {
        const float progress = status.totalBytes ?
            100.f * static_cast<float>(status.downloadedBytes) / static_cast<float>(status.totalBytes) : 0.f;
        m_progress->ForceSetProgressPos(std::clamp(progress, 0.f, 100.f));
    }
    m_update->Enable(status.state == State::Available || status.state == State::Failed);
}

void CUIModsWnd::AskForRestart()
{
    // only from this window and only when it has the input: a box opened under another
    // dialog would answer to nobody
    if (!IsShown() || GetHolder()->TopInputReceiver() != this)
        return;
    ModUpdateService::AcknowledgeRestartPrompt();
    if (!m_restartBox)
        return;
    m_restartBox->SetText(mods_text("st_mods_restart_prompt").c_str());
    m_restartBox->ShowDialog(false);
}

void CUIModsWnd::OnUpdate(CUIWindow*, void*)
{
    if (m_selected < m_rows.size())
        ModUpdateService::StartUpdate(m_rows[m_selected].id.c_str());
}

void CUIModsWnd::OnUpdateAll(CUIWindow*, void*) { ModUpdateService::StartUpdateAll(); }

void CUIModsWnd::OnWebsite(CUIWindow*, void*)
{
    if (m_selected < m_rows.size())
        ModUpdateService::OpenWebsite(m_rows[m_selected].id.c_str());
}

void CUIModsWnd::OnBack(CUIWindow*, void*) { HideDialog(); }

void CUIModsWnd::OnRestartYes(CUIWindow*, void*)
{
    if (!ModUpdateService::RestartGame())
        Msg("! [mods] could not restart the game - the updates are applied by the next start");
}
