////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_sound_data_visitor.h
//	Created 	: 02.02.2005
//  Modified 	: 02.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker sound data visitor
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "sound_user_data_visitor.h"

class CAI_Stalker;
class CStalkerSoundData;

class CStalkerSoundDataVisitor : public CSound_UserDataVisitor
{
private:
    CAI_Stalker* m_object;
    // Context of heard(), the call after the hearing threshold; null while the early call visits.
    const Fvector* m_position{};
    float m_power{};
    float m_masking{};
    // The early call already ran the stock transfer for this sound event.
    CStalkerSoundData* m_stock_data{};

    void stock_visit(CStalkerSoundData* data);

public:
    IC CStalkerSoundDataVisitor(CAI_Stalker* object);
    virtual ~CStalkerSoundDataVisitor();
    virtual void visit(CStalkerSoundData* data);
    // The owner heard the sound of this user data above its hearing threshold.
    void heard(CSound_UserData& data, const Fvector& position, float power, float masking);
    IC CAI_Stalker& object() const;
};

#include "stalker_sound_data_visitor_inline.h"
