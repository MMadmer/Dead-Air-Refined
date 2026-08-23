#include "StdAfx.h"
#include "pp_effector_distance.h"

////////////////////////////////////////////////////////////////////////////////////
// CPPEffectorDistance
////////////////////////////////////////////////////////////////////////////////////
void CPPEffectorDistance::load(LPCSTR section)
{
    inherited::load(section);

    m_r_min_perc = pSettings->r_float(section, "radius_min");
    m_r_max_perc = pSettings->r_float(section, "radius_max");

    // Equal radii put a zero in the update_factor denominator; the NaN then passes the clamp (every
    // comparison with NaN is false) straight into set_factor, silently.
    if (m_r_min_perc >= m_r_max_perc)
    {
        Msg("! Distance effector [%s]: radius_min %.3f is not below radius_max %.3f, pulled apart", section,
            m_r_min_perc, m_r_max_perc);
        m_r_min_perc = m_r_max_perc - EPS_L;
    }
}

bool CPPEffectorDistance::check_completion() { return (m_dist > m_radius * m_r_max_perc); }
bool CPPEffectorDistance::check_start_conditions() { return (m_dist < m_radius * m_r_max_perc); }
void CPPEffectorDistance::update_factor()
{
    float factor;
    factor = (m_radius * m_r_max_perc - m_dist) / (m_radius * m_r_max_perc - m_radius * m_r_min_perc);
    clamp(factor, 0.01f, 1.0f);

    m_effector->set_factor(factor);
}

CPPEffectorControlled* CPPEffectorDistance::create_effector() { return xr_new<CPPEffectorControlled>(this, m_state); }
