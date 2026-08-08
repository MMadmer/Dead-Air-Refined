////////////////////////////////////////////////////////////////////////////
//	Module 		: level_path_builder.h
//  Modified 	: 21.02.2005
//  Modified 	: 21.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Level path builder
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "movement_manager.h"
#include "level_path_manager.h"
#include "detail_path_builder.h"

class CLevelPathBuilder : public CDetailPathBuilder
{
private:
    typedef CDetailPathBuilder inherited;

private:
    Fvector m_temp;
    u32 m_start_vertex_id;
    u32 m_dest_vertex_id;
    const Fvector* m_precise_position;
    u32 m_last_fail_time;
    bool m_extrapolate_path;
    bool m_use_delay_after_fail;
    u32 m_consecutive_fails;
    u32 m_fail_backoff_end;

private:
    enum
    {
        time_to_wait_after_fail = u32(2000),
        // sustained identical-failure backoff for objects with the delay above disabled
        consecutive_fails_threshold = u32(20),
        time_to_wait_after_sustained_fails = u32(500),
    };

    IC bool fail_backoff_active() const
    {
        // wrap-safe: an armed deadline is never further than the backoff interval ahead
        return (s32(m_fail_backoff_end - Device.dwTimeGlobal) > 0);
    }

public:
    IC CLevelPathBuilder(CMovementManager* object)
        : inherited(object), m_start_vertex_id(u32(-1)), m_dest_vertex_id(u32(-1)), m_last_fail_time(0),
          m_use_delay_after_fail(true), m_consecutive_fails(0), m_fail_backoff_end(0)
    {
    }

    IC const u32& dest_vertex_id() const { return (m_dest_vertex_id); }
    IC void use_delay_after_fail(bool const value) { m_use_delay_after_fail = value; }
    IC void setup(
        const u32& start_vertex_id, const u32& dest_vertex_id, bool extrapolate_path, const Fvector* precise_position)
    {
        // a changed request is not the same failure anymore: retry it immediately
        if (start_vertex_id != m_start_vertex_id || dest_vertex_id != m_dest_vertex_id)
        {
            m_consecutive_fails = 0;
            m_fail_backoff_end = 0;
        }

        VERIFY(ai().level_graph().valid_vertex_id(start_vertex_id));
        m_start_vertex_id = start_vertex_id;

        VERIFY(ai().level_graph().valid_vertex_id(dest_vertex_id));
        m_dest_vertex_id = dest_vertex_id;

        m_extrapolate_path = extrapolate_path;
        if (!precise_position)
            m_precise_position = 0;
        else
        {
            m_temp = *precise_position;
            m_precise_position = &m_temp;
        }
    }

    void register_to_process()
    {
        // resting after sustained failures: skip without arming the wait flag, so
        // update_path() keeps polling and the attempt resumes once the rest expires
        if (fail_backoff_active())
            return;

        m_object->m_wait_for_distributed_computation = true;
        if (Device.dwTimeGlobal < m_last_fail_time + time_to_wait_after_fail)
            return;

        Device.add_to_seq_parallel(
            fastdelegate::FastDelegate0<>(this, &CLevelPathBuilder::process), &m_object->object());
    }

    void process_impl()
    {
        m_object->m_wait_for_distributed_computation = false;
        m_object->level_path().build_path(m_start_vertex_id, m_dest_vertex_id);

        if (m_object->level_path().failed())
        {
            if (m_use_delay_after_fail)
                m_last_fail_time = Device.dwTimeGlobal;
            // stalkers disable the delay above; after enough identical failures in a row
            // rest briefly instead of rerunning the same worst-case search every frame
            else if (++m_consecutive_fails >= consecutive_fails_threshold)
                m_fail_backoff_end = Device.dwTimeGlobal + time_to_wait_after_sustained_fails;

            m_object->m_path_state = CMovementManager::ePathStateBuildLevelPath;
            return;
        }

        m_consecutive_fails = 0;
        m_fail_backoff_end = 0;

        m_object->level_path().select_intermediate_vertex();

        m_object->m_path_state = CMovementManager::ePathStateBuildDetailPath;

        m_object->detail().set_state_patrol_path(m_extrapolate_path);
        m_object->detail().set_start_position(m_object->object().Position());
        m_object->detail().set_start_direction(Fvector().setHP(-m_object->m_body.current.yaw, 0));

        if (m_precise_position)
            m_object->detail().set_dest_position(*m_precise_position);

        inherited::setup(m_object->level_path().path(), m_object->level_path().intermediate_index());
        inherited::process_impl(false);
    }

    void process()
    {
        if (Device.dwTimeGlobal < m_last_fail_time + time_to_wait_after_fail)
            return;

        m_object->build_level_path();
    }

    IC void remove()
    {
        if (m_object->m_wait_for_distributed_computation)
            m_object->m_wait_for_distributed_computation = false;

        // restrictions change/destroy invalidates the tracked failure streak
        m_consecutive_fails = 0;
        m_fail_backoff_end = 0;

        Device.remove_from_seq_parallel(fastdelegate::FastDelegate0<>(this, &CLevelPathBuilder::process));
    }
};
