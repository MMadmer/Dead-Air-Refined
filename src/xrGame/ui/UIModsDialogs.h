// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "UIDialogWnd.h"
#include "xrUICore/Callbacks/UIWndCallback.h"
#include "xrUICore/Static/UIShape.h"

class CUIEditBox;
class CUIStatic;
class CUIXml;

// A question in the Mods window's own look: a caption, a text, an optional line of input and two
// buttons. Escape is Cancel, and so is Enter unless the prompt asks for nothing that loses data.
class CUIModsPrompt final : public CUIDialogWnd, public CUIWndCallback
{
    using inherited = CUIDialogWnd;

public:
    // called with no window: the answer, not the click that asked
    using Callback = CUIWndCallback::void_function;

    struct Question
    {
        xr_string caption, text; // UI code page
        xr_string accept;        // empty: an information box with one button
        xr_string cancel;        // empty: Cancel
        bool danger{};           // the accept button in the warning look
        bool enterAccepts{};     // nothing is lost by answering yes
        bool input{};            // a line of input, masked
        Callback onAccept;
    };

    CUIModsPrompt();
    bool Init(CUIXml& xml);
    void Ask(Question question);
    pcstr Input() const;
    const xr_string& Caption() const { return m_question.caption; }
    // what the buttons and the keyboard do, for the console
    void Answer(bool accept);
    void SetInput(pcstr text);

    void SendMessage(CUIWindow* window, s16 message, void* data) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboardAction) override;
    pcstr GetDebugType() override { return "CUIModsPrompt"; }

private:
    void OnAccept(CUIWindow*, void*);
    void OnCancel(CUIWindow*, void*);
    void FitToText();

    CUIShape* m_shadow{};
    CUIShape* m_frame{};
    CUIStatic* m_caption{};
    CUIStatic* m_text{};
    CUIShape* m_inputBack{};
    CUIEditBox* m_input{};
    CUIShapeButton* m_accept{};
    CUIShapeButton* m_cancel{};
    SUIShapeStyle m_acceptLooks[CUIShapeButton::lookCount];
    SUIShapeStyle m_dangerLooks[CUIShapeButton::lookCount];
    u32 m_acceptText[CUIShapeButton::lookCount]{};
    u32 m_dangerText[CUIShapeButton::lookCount]{};
    Fvector2 m_acceptPos{}, m_cancelPos{};
    // the layout's distances from the frame's top, kept while the frame follows the text
    float m_captionTop{}, m_textTop{}, m_shadowGrow{}, m_bottom{}, m_minHeight{};
    Question m_question;
};

// Writing a review (MOD_CATALOG 12): stars picked by a click, a text of 30 characters at least, a
// name if the player wants one. The review is signed and paid for with a proof of work on a
// worker, so the dialog stays until the catalog client says how it went.
class CUIModsReview final : public CUIDialogWnd, public CUIWndCallback
{
    using inherited = CUIDialogWnd;

public:
    using Callback = fastdelegate::FastDelegate0<void>;

    CUIModsReview();
    bool Init(CUIXml& xml);
    // `name` is the module's name in the UI code page
    void Open(pcstr moduleId, pcstr name);
    const xr_string& ModuleId() const { return m_module; }
    // what the console hands over for a test: the stars and the text, UI code page
    void Fill(u32 stars, pcstr text);
    void Send() { OnSend(nullptr, nullptr); }
    // called once the review is on its way
    Callback onSent;

    void Update() override;
    void SendMessage(CUIWindow* window, s16 message, void* data) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboardAction) override;
    pcstr GetDebugType() override { return "CUIModsReview"; }

private:
    void OnSend(CUIWindow*, void*);
    void OnCancel(CUIWindow*, void*);
    bool Valid() const;

    CUIStatic* m_caption{};
    CUIShapeButton* m_stars[5]{};
    CUIStatic* m_starsLabel{};
    CUIEditBox* m_name{};
    CUIEditBox* m_text{};
    CUIStatic* m_counter{};
    CUIStatic* m_note{};
    CUIStatic* m_status{};
    CUIShapeButton* m_send{};
    CUIShapeButton* m_cancel{};
    u32 m_counterColor{};
    u32 m_counterShortColor{};
    u32 m_errorColor{};
    xr_string m_module;
    u32 m_rating{};
    bool m_sending{};
};
