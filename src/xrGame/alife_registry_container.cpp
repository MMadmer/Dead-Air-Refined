////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_registry_container.cpp
//	Created 	: 01.07.2004
//  Modified 	: 01.07.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife registry container class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_registry_container.h"
#include "Common/object_interfaces.h"
#include "alife_space.h"
#include "Common/object_type_traits.h"

template <typename TContainer, typename TList>
struct RegistryHelper;

template <typename TContainer>
struct RegistryHelper<TContainer, Loki::NullType>
{
    static void Load(TContainer*, IReader&) {};
    static bool SaveAt(TContainer*, IWriter&, u32, u32&) { return false; }
    static constexpr u32 serializableCount = 0;
};

template <typename TContainer, typename Head, typename Tail>
struct RegistryHelper<TContainer, Loki::Typelist<Head, Tail>>
{
    static constexpr bool isSerializable =
        object_type_traits::is_base_and_derived<ISerializable, Head>::value &&
        !std::is_same_v<Head, CKnownContactsRegistry> &&
        !std::is_same_v<Head, CEncyclopediaRegistry>;
    static constexpr u32 serializableCount =
        RegistryHelper<TContainer, Tail>::serializableCount + (isSerializable ? 1u : 0u);

    static bool SaveAt(TContainer* self, IWriter& writer, u32 target, u32& index)
    {
        if (RegistryHelper<TContainer, Tail>::SaveAt(self, writer, target, index))
            return true;
        if constexpr (isSerializable)
        {
            if (index++ == target)
            {
                self->Head::save(writer);
                return true;
            }
        }
        return false;
    };

    static void Load(TContainer* self, IReader& reader)
    {
        RegistryHelper<TContainer, Tail>::Load(self, reader);
        if constexpr (isSerializable)
        {
            Msg("~ Registry load %s at %u", typeid(Head).name(), reader.tell());
            self->Head::load(reader);
            Msg("~ Registry loaded %s at %u", typeid(Head).name(), reader.tell());
        }
    }
};

void CALifeRegistryContainer::load(IReader& file_stream)
{
    R_ASSERT2(file_stream.find_chunk(REGISTRY_CHUNK_DATA), "Can't find chunk REGISTRY_CHUNK_DATA!");
    RegistryHelper<CALifeRegistryContainer, TYPE_LIST>::Load(this, file_stream);
}

void CALifeRegistryContainer::save(IWriter& memory_stream)
{
    SaveState state;
    begin_save(memory_stream, state);
    while (!continue_save(memory_stream, state, flt_max))
    {
    }
}

void CALifeRegistryContainer::begin_save(IWriter& memory_stream, SaveState& state)
{
    state = {};
    memory_stream.open_chunk(REGISTRY_CHUNK_DATA);
    state.initialized = true;
}

bool CALifeRegistryContainer::continue_save(
    IWriter& memory_stream, SaveState& state, float budgetMilliseconds)
{
    if (!state.initialized)
        return false;
    if (state.completed)
        return true;
    if (!(budgetMilliseconds > 0.f))
        return false;

    using Helper = RegistryHelper<CALifeRegistryContainer, TYPE_LIST>;
    const bool unlimitedBudget = budgetMilliseconds == flt_max;
    CTimer budgetTimer;
    if (!unlimitedBudget)
        budgetTimer.Start();

    while (state.phase < Helper::serializableCount)
    {
        u32 index = 0;
        R_ASSERT2(Helper::SaveAt(this, memory_stream, state.phase, index),
            "Invalid ALife registry save phase");
        ++state.phase;
        if (!unlimitedBudget && budgetTimer.GetElapsed_sec() * 1000.f >= budgetMilliseconds)
            return false;
    }

    memory_stream.close_chunk();
    state.completed = true;
    return true;
}
