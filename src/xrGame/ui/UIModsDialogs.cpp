// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UIModsDialogs.h"

#include "ModCatalog.h"
#include "UIHelper.h"
#include "UIXmlInit.h"
#include "xrEngine/xr_input.h"
#include "xrUICore/EditBox/UIEditBox.h"
#include "xrUICore/Static/UIStatic.h"

namespace
{
shared_str dialog_text(pcstr id) { return StringTable().translate(id); }

CUIShape* dialog_shape(CUIXml& xml, pcstr path, CUIWindow* parent)
{
    auto* shape = xr_new<CUIShape>();
    CUIXmlInit::InitWindow(xml, path, 0, shape);
    UIShape::Read(xml, path, 0, shape->style);
    shape->SetAutoDelete(true);
    parent->AttachChild(shape);
    return shape;
}

CUIShapeButton* dialog_button(CUIXml& xml, pcstr path, CUIWindow* parent)
{
    auto* button = xr_new<CUIShapeButton>();
    CUIXmlInit::InitButton(xml, path, 0, button);
    button->InitLooks(xml, path, 0);
    button->SetAutoDelete(true);
    parent->AttachChild(button);
    return button;
}

// The edit boxes type Windows-1251; a review travels as UTF-8.
xr_string dialog_utf8(pcstr text)
{
    const int length = text ? static_cast<int>(xr_strlen(text)) : 0;
    if (!length)
        return {};
    const int wide = MultiByteToWideChar(1251, 0, text, length, nullptr, 0);
    std::wstring buffer(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(1251, 0, text, length, buffer.data(), wide);
    const int narrow = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, nullptr, 0, nullptr, nullptr);
    xr_string out(static_cast<size_t>(narrow), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, out.data(), narrow, nullptr, nullptr);
    return out;
}

// Printable characters, the way the reader counts them: what the edit box holds, trimmed.
size_t dialog_length(pcstr text)
{
    std::string_view view(text ? text : "");
    while (!view.empty() && static_cast<u8>(view.front()) <= ' ')
        view.remove_prefix(1);
    while (!view.empty() && static_cast<u8>(view.back()) <= ' ')
        view.remove_suffix(1);
    return view.size();
}
} // namespace

// ---- the prompt -------------------------------------------------------------------------------------

CUIModsPrompt::CUIModsPrompt() : CUIDialogWnd(CUIModsPrompt::GetDebugType()) { m_bWorkInPause = true; }

bool CUIModsPrompt::Init(CUIXml& xml)
{
    CUIXmlInit::InitWindow(xml, "prompt", 0, this);
    dialog_shape(xml, "prompt:shade", this);
    m_shadow = dialog_shape(xml, "prompt:shadow", this);
    m_frame = dialog_shape(xml, "prompt:frame", this);
    m_caption = UIHelper::CreateStatic(xml, "prompt:caption", this);
    m_text = UIHelper::CreateStatic(xml, "prompt:text", this);
    m_inputBack = dialog_shape(xml, "prompt:input_back", this);
    m_input = UIHelper::CreateEditBox(xml, "prompt:input", this);
    m_cancel = dialog_button(xml, "prompt:cancel", this);
    m_accept = dialog_button(xml, "prompt:accept", this);
    std::copy(std::begin(m_accept->looks), std::end(m_accept->looks), m_acceptLooks);
    std::copy(std::begin(m_accept->textColors), std::end(m_accept->textColors), m_acceptText);

    // the warning look replaces the accept button's own when the answer loses something
    UIShape::ReadLooks(xml, "prompt:accept_danger", 0, m_dangerLooks, m_dangerText, m_acceptText[0]);
    m_acceptPos = m_accept->GetWndPos();
    m_cancelPos = m_cancel->GetWndPos();
    const float top = m_frame->GetWndPos().y;
    m_captionTop = m_caption->GetWndPos().y - top;
    m_textTop = m_text->GetWndPos().y - top;
    m_shadowGrow = m_shadow->GetHeight() - m_frame->GetHeight();
    m_bottom = top + m_frame->GetHeight() - (m_accept->GetWndPos().y + m_accept->GetHeight());
    m_minHeight = xml.ReadAttribFlt("prompt:frame", 0, "min_height", 150.f);

    Register(m_accept);
    Register(m_cancel);
    AddCallback(m_accept, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIModsPrompt::OnAccept));
    AddCallback(m_cancel, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIModsPrompt::OnCancel));
    return true;
}

void CUIModsPrompt::Ask(Question question)
{
    m_question = std::move(question);
    m_caption->SetText(m_question.caption.c_str());
    m_text->SetText(m_question.text.c_str());
    m_input->Show(m_question.input);
    m_inputBack->Show(m_question.input);
    m_input->SetText("");

    const bool info = m_question.accept.empty();
    m_cancel->Show(!info);
    m_cancel->SetText(m_question.cancel.empty() ? dialog_text("st_mods_cancel").c_str() : m_question.cancel.c_str());
    m_accept->SetText(info ? dialog_text("st_mods_ok").c_str() : m_question.accept.c_str());
    std::copy(std::begin(m_question.danger ? m_dangerLooks : m_acceptLooks),
        std::end(m_question.danger ? m_dangerLooks : m_acceptLooks), m_accept->looks);
    std::copy(std::begin(m_question.danger ? m_dangerText : m_acceptText),
        std::end(m_question.danger ? m_dangerText : m_acceptText), m_accept->textColors);

    FitToText();
    if (!IsShown())
        ShowDialog(true);
    if (m_question.input)
        m_input->CaptureFocus(true);
}

// The box is as tall as its text: the frame, its shadow, the input and the buttons follow it,
// and the whole stays in the middle of the screen.
void CUIModsPrompt::FitToText()
{
    m_text->AdjustHeightToText();
    constexpr float gap = 16.f;
    float height = m_textTop + m_text->GetHeight() + gap;
    if (m_question.input)
        height += m_inputBack->GetHeight() + gap;
    height = std::max(height + m_accept->GetHeight() + m_bottom, m_minHeight);
    const float top = std::floor((UI_BASE_HEIGHT - height) * 0.5f);

    const auto place = [](CUIWindow* window, float y) { window->SetWndPos(Fvector2().set(window->GetWndPos().x, y)); };
    place(m_frame, top);
    m_frame->SetHeight(height);
    place(m_shadow, top - m_shadowGrow * 0.25f);
    m_shadow->SetHeight(height + m_shadowGrow);
    place(m_caption, top + m_captionTop);
    place(m_text, top + m_textTop);
    float y = top + m_textTop + m_text->GetHeight() + gap;
    if (m_question.input)
    {
        place(m_inputBack, y);
        place(m_input, y);
        y += m_inputBack->GetHeight() + gap;
    }
    const float buttons = top + height - m_bottom - m_accept->GetHeight();
    m_acceptPos.y = buttons;
    m_cancelPos.y = buttons;
    // alone in an information box, the accept button keeps its right-hand place
    place(m_cancel, buttons);
    place(m_accept, buttons);
}

pcstr CUIModsPrompt::Input() const { return m_input->GetText(); }

void CUIModsPrompt::SendMessage(CUIWindow* window, s16 message, void* data)
{
    CUIWndCallback::OnEvent(window, message, data);
    inherited::SendMessage(window, message, data);
}

bool CUIModsPrompt::OnKeyboardAction(int dik, EUIMessages keyboardAction)
{
    if (keyboardAction == WINDOW_KEY_PRESSED)
    {
        if (IsBinded(kQUIT, dik))
        {
            OnCancel(nullptr, nullptr);
            return true;
        }
        // Enter answers with the safe button unless nothing can be lost
        if (dik == SDL_SCANCODE_RETURN || dik == SDL_SCANCODE_KP_ENTER)
        {
            if (m_question.enterAccepts || m_question.accept.empty())
                OnAccept(nullptr, nullptr);
            else
                OnCancel(nullptr, nullptr);
            return true;
        }
    }
    return inherited::OnKeyboardAction(dik, keyboardAction);
}

void CUIModsPrompt::OnAccept(CUIWindow*, void*)
{
    // the answer may open the next question in this same box
    const Callback accept = m_question.accept.empty() ? Callback() : m_question.onAccept;
    HideDialog();
    if (accept)
        accept(nullptr, nullptr);
}

void CUIModsPrompt::OnCancel(CUIWindow*, void*) { HideDialog(); }

void CUIModsPrompt::Answer(bool accept)
{
    if (accept)
        OnAccept(nullptr, nullptr);
    else
        OnCancel(nullptr, nullptr);
}

void CUIModsPrompt::SetInput(pcstr text) { m_input->SetText(text ? text : ""); }

// ---- the review ---------------------------------------------------------------------------------------

CUIModsReview::CUIModsReview() : CUIDialogWnd(CUIModsReview::GetDebugType()) { m_bWorkInPause = true; }

bool CUIModsReview::Init(CUIXml& xml)
{
    CUIXmlInit::InitWindow(xml, "review", 0, this);
    dialog_shape(xml, "review:shade", this);
    dialog_shape(xml, "review:shadow", this);
    dialog_shape(xml, "review:frame", this);
    m_caption = UIHelper::CreateStatic(xml, "review:caption", this);
    m_starsLabel = UIHelper::CreateStatic(xml, "review:stars_label", this);
    // five stars in a row from one template: the first one's place, then a step
    const float step = xml.ReadAttribFlt("review:star", 0, "step", 24.f);
    for (u32 star = 0; star != 5; ++star)
    {
        m_stars[star] = dialog_button(xml, "review:star", this);
        Fvector2 position = m_stars[star]->GetWndPos();
        position.x += step * star;
        m_stars[star]->SetWndPos(position);
        Register(m_stars[star]);
    }
    UIHelper::CreateStatic(xml, "review:name_label", this);
    dialog_shape(xml, "review:name_back", this);
    m_name = UIHelper::CreateEditBox(xml, "review:name", this);
    UIHelper::CreateStatic(xml, "review:text_label", this);
    dialog_shape(xml, "review:text_back", this);
    m_text = UIHelper::CreateEditBox(xml, "review:text", this);
    m_counter = UIHelper::CreateStatic(xml, "review:counter", this);
    m_counterColor = m_counter->GetTextColor();
    m_counterShortColor = UIShape::ParseColor(xml.ReadAttrib("review:counter", 0, "short", nullptr), m_counterColor);
    m_note = UIHelper::CreateStatic(xml, "review:note", this);
    m_status = UIHelper::CreateStatic(xml, "review:status", this);
    m_errorColor = UIShape::ParseColor(xml.ReadAttrib("review:status", 0, "error", nullptr), 0xffdc645a);
    m_cancel = dialog_button(xml, "review:cancel", this);
    m_send = dialog_button(xml, "review:send", this);
    m_name->SetNextFocusCapturer(m_text);
    m_text->SetNextFocusCapturer(m_name);

    Register(m_send);
    Register(m_cancel);
    AddCallback(m_send, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIModsReview::OnSend));
    AddCallback(m_cancel, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIModsReview::OnCancel));
    return true;
}

void CUIModsReview::Open(pcstr moduleId, pcstr name)
{
    m_module = moduleId ? moduleId : "";
    m_rating = 0;
    m_sending = false;
    string512 caption{};
    xr_sprintf(caption, sizeof(caption), dialog_text("st_mods_review_caption").c_str(), name ? name : moduleId);
    m_caption->SetText(caption);
    m_name->SetText("");
    m_text->SetText("");
    m_status->SetText("");
    if (!IsShown())
        ShowDialog(true);
    m_text->CaptureFocus(true);
}

void CUIModsReview::Fill(u32 stars, pcstr text)
{
    m_rating = std::min<u32>(stars, 5);
    m_text->SetText(text ? text : "");
}

bool CUIModsReview::Valid() const
{
    const size_t length = dialog_length(m_text->GetText());
    return m_rating >= 1 && length >= ModCatalog::ReviewMinimum && length <= ModCatalog::ReviewMaximum;
}

void CUIModsReview::Update()
{
    inherited::Update();

    // the stars show what a click would give while the cursor is over them
    u32 shown = m_rating;
    for (u32 star = 0; star != 5; ++star)
        if (m_stars[star]->CursorOverWindow() && !m_sending)
            shown = star + 1;
    for (u32 star = 0; star != 5; ++star)
        m_stars[star]->checked = star < shown;

    const size_t length = dialog_length(m_text->GetText());
    string64 counter{};
    xr_sprintf(counter, sizeof(counter), dialog_text("st_mods_review_counter").c_str(), static_cast<u32>(length),
        static_cast<u32>(ModCatalog::ReviewMinimum));
    if (0 != xr_strcmp(m_counter->GetText(), counter))
        m_counter->SetText(counter);
    m_counter->SetTextColor(length < ModCatalog::ReviewMinimum ? m_counterShortColor : m_counterColor);

    if (m_sending)
    {
        const ModCatalog::PostStatus post = ModCatalog::GetPostStatus();
        if (post.state == ModCatalog::PostState::Sent)
        {
            m_sending = false;
            HideDialog();
            if (onSent)
                onSent();
            return;
        }
        if (post.state == ModCatalog::PostState::Failed)
        {
            m_sending = false;
            string512 text{};
            xr_sprintf(text, sizeof(text), dialog_text("st_mods_review_failed").c_str(), post.error.c_str());
            m_status->SetText(text);
            m_status->SetTextColor(m_errorColor);
        }
    }
    m_send->Enable(Valid() && !m_sending);
}

void CUIModsReview::SendMessage(CUIWindow* window, s16 message, void* data)
{
    if (message == BUTTON_CLICKED && !m_sending)
        for (u32 star = 0; star != 5; ++star)
            if (window == m_stars[star])
                m_rating = star + 1;
    CUIWndCallback::OnEvent(window, message, data);
    inherited::SendMessage(window, message, data);
}

bool CUIModsReview::OnKeyboardAction(int dik, EUIMessages keyboardAction)
{
    if (keyboardAction == WINDOW_KEY_PRESSED && IsBinded(kQUIT, dik) && !m_sending)
    {
        HideDialog();
        return true;
    }
    return inherited::OnKeyboardAction(dik, keyboardAction);
}

void CUIModsReview::OnSend(CUIWindow*, void*)
{
    if (!Valid() || m_sending)
        return;
    xr_string error;
    if (!ModCatalog::PostReview(m_module.c_str(), m_rating, dialog_utf8(m_text->GetText()), dialog_utf8(m_name->GetText()), error))
    {
        string512 text{};
        xr_sprintf(text, sizeof(text), dialog_text("st_mods_review_failed").c_str(), error.c_str());
        m_status->SetText(text);
        m_status->SetTextColor(m_errorColor);
        return;
    }
    m_sending = true;
    m_status->SetText(dialog_text("st_mods_review_sending").c_str());
    m_status->SetTextColor(m_counterColor);
}

void CUIModsReview::OnCancel(CUIWindow*, void*)
{
    if (!m_sending)
        HideDialog();
}
