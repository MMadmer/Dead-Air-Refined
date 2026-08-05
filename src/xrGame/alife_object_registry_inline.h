////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_object_registry_штдшту.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife object registry inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC void CALifeObjectRegistry::add(CSE_ALifeDynamicObject* object)
{
    const auto [iterator, inserted] = m_objects.emplace(object->ID, object);
    if (!inserted)
    {
        THROW2(iterator->second == object,
            "The specified object is already presented in the Object Registry!");
        THROW2(iterator->second != object,
            "Object with the specified ID is already presented in the Object Registry!");
    }

    m_objectIndex[object->ID] = object;
    index_save_extension_object(object);
}

IC void CALifeObjectRegistry::remove(const ALife::_OBJECT_ID& id, bool no_assert)
{
    OBJECT_REGISTRY::iterator I = m_objects.find(id);
    if (I == m_objects.end())
    {
        THROW2(no_assert, "The specified object hasn't been found in the Object Registry!");
        return;
    }

    m_objectIndex[id] = nullptr;
    const size_t typeWord = id / std::numeric_limits<u64>::digits;
    const u64 typeBit = u64{1} << (id % std::numeric_limits<u64>::digits);
    for (auto& typeWords : m_saveExtensionObjectTypes)
        typeWords[typeWord] &= ~typeBit;
    m_objects.erase(I);
}

IC CSE_ALifeDynamicObject* CALifeObjectRegistry::object(const ALife::_OBJECT_ID& id, bool no_assert) const
{
    START_PROFILE("ALife/objects::object")
    CSE_ALifeDynamicObject* object = m_objectIndex[id];

    if (!object)
    {
#ifdef DEBUG
        if (!no_assert)
            Msg("There is no object with id %d!", id);
#endif
        THROW2(no_assert, "Specified object hasn't been found in the object registry!");
        return (0);
    }

    return object;
    STOP_PROFILE
}

IC const CALifeObjectRegistry::OBJECT_REGISTRY& CALifeObjectRegistry::objects() const { return (m_objects); }
IC CALifeObjectRegistry::OBJECT_REGISTRY& CALifeObjectRegistry::objects() { return (m_objects); }
