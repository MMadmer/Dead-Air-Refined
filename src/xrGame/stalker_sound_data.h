////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_sound_data.h
//	Created 	: 02.02.2005
//  Modified 	: 02.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker sound data
////////////////////////////////////////////////////////////////////////////

#pragma once

class CAI_Stalker;

class CStalkerSoundData : public CSound_UserData
{
private:
    CAI_Stalker* m_object;
    // Internal sound type of the collection this data belongs to, set by the comms lane when the sounds load;
    // u32(-1) for sounds added later (script combat sounds).
    u32 m_bark{u32(-1)};

public:
    IC CStalkerSoundData(CAI_Stalker* object);
    virtual ~CStalkerSoundData();
    virtual void invalidate();
    virtual void accept(CSound_UserDataVisitor* visitor);
    IC CAI_Stalker& object() const;
    void bark(u32 value) { m_bark = value; }
    u32 bark() const { return m_bark; }
};

#include "stalker_sound_data_inline.h"
