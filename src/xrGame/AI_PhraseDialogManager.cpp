///////////////////////////////////////////////////////////////
// AI_PhraseDialogManager.cpp
// Класс, от которого наследуются NPC персонажи, ведущие диалог
// с актером
//
///////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "AI_PhraseDialogManager.h"
#include "PhraseDialog.h"
#include "InventoryOwner.h"
#include "character_info.h"
#include "GameObject.h"
#include "relation_registry.h"

CAI_PhraseDialogManager::CAI_PhraseDialogManager() = default;

void CAI_PhraseDialogManager::ReceivePhrase(DIALOG_SHARED_PTR& phrase_dialog)
{
    AnswerPhrase(phrase_dialog);
    CPhraseDialogManager::ReceivePhrase(phrase_dialog);
}
#include "UIGameSP.h"
#include "Level.h"
#include "ui/UITalkWnd.h"

void CAI_PhraseDialogManager::AnswerPhrase(DIALOG_SHARED_PTR& phrase_dialog)
{
    CInventoryOwner* pInvOwner = smart_cast<CInventoryOwner*>(this);
    THROW(pInvOwner);
    CGameObject* pOthersGO = smart_cast<CGameObject*>(phrase_dialog->OurPartner(this));
    THROW(pOthersGO);
    CInventoryOwner* pOthersIO = smart_cast<CInventoryOwner*>(pOthersGO);
    THROW(pOthersIO);

    if (!phrase_dialog->IsFinished())
    {
        // Nothing left to answer with: every successor was filtered out by its precondition.
        // The selection below indexes the candidate list unconditionally, so an empty list used
        // to be an access violation - any dialog, from any addon, could take the game down here.
        if (phrase_dialog->PhraseList().empty())
        {
            Msg("! dialog [%s]: %s has no available phrase to answer with, the dialog is finished",
                phrase_dialog->GetDialogID().c_str(), pInvOwner->Name());
            phrase_dialog->SetFinished();
            return;
        }

        CHARACTER_GOODWILL attitude = RELATION_REGISTRY().GetAttitude(pOthersIO, pInvOwner);

        xr_vector<int> phrases;
        //если не найдем более подходяещей выводим фразу
        //последнюю из списка (самую грубую)
        int phrase_num = phrase_dialog->PhraseList().size() - 1;
        CHARACTER_GOODWILL phrase_goodwill = phrase_dialog->PhraseList()[phrase_num]->GoodwillLevel();
        // Both loops used to index [phrase_num] instead of [i], so the goodwill under test never
        // changed: the first loop always compared the LAST phrase and the second compared the
        // chosen phrase with itself. Every stock dialog gives all its phrases the same goodwill
        // (-10000), which hides it - but a dialog with distinct goodwill levels left `phrases`
        // empty and the pick below read past the end of it.
        for (u32 i = 0; i < phrase_dialog->PhraseList().size(); ++i)
        {
            const CHARACTER_GOODWILL goodwill = phrase_dialog->PhraseList()[i]->GoodwillLevel();
            if (attitude >= goodwill)
            {
                phrase_num = i;
                phrase_goodwill = goodwill;
                break;
            }
        }

        for (u32 i = 0; i < phrase_dialog->PhraseList().size(); i++)
        {
            if (phrase_goodwill == phrase_dialog->PhraseList()[i]->GoodwillLevel())
                phrases.push_back(i);
        }

        phrase_num = phrases[Random.randI(0, phrases.size())];

        shared_str phrase_id = phrase_dialog->PhraseList()[phrase_num]->GetID();

        CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(CurrentGameUI());
        pGameSP->TalkMenu->AddAnswer(phrase_dialog->GetPhraseText(phrase_id), pInvOwner->Name());

        CPhraseDialogManager::SayPhrase(phrase_dialog, phrase_id);
    }
}

void CAI_PhraseDialogManager::SetStartDialog(shared_str phrase_dialog) { m_sStartDialog = phrase_dialog; }
void CAI_PhraseDialogManager::SetDefaultStartDialog(shared_str phrase_dialog) { m_sDefaultStartDialog = phrase_dialog; }
void CAI_PhraseDialogManager::RestoreDefaultStartDialog() { m_sStartDialog = m_sDefaultStartDialog; }
void CAI_PhraseDialogManager::UpdateAvailableDialogs(CPhraseDialogManager* partner)
{
    m_AvailableDialogs.clear();
    m_CheckedDialogs.clear();

    if (m_sStartDialog.c_str())
        inherited::AddAvailableDialog(m_sStartDialog.c_str(), partner);
    inherited::AddAvailableDialog("hello_dialog", partner);

    inherited::UpdateAvailableDialogs(partner);
}
