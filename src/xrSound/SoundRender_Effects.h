#pragma once

class CSoundRender_Environment;

class XR_NOVTABLE CSoundRender_Effects
{
public:
    virtual ~CSoundRender_Effects() = default;

    virtual bool initialized() = 0;

    virtual void set_listener(const CSoundRender_Environment& env) = 0;
    virtual void get_listener(CSoundRender_Environment& env) = 0;

    virtual void commit() = 0;

    // The highs a source loses through the walls (mB, 0 - none).
    virtual void set_source_occlusion(u32 /*source*/, int /*occlusion*/) {}
};
