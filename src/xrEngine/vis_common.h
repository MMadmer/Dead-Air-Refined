#pragma once
#include "xrCore/_sphere.h"
#include "xrCore/_fbox.h"

#include "vis_object_data.h" //--#SM+#--

#pragma pack(push, 4)
struct vis_data
{
private:
    vis_object_data obj_data_self; //--#SM+#-- Свои собственные объектные данные [personal shaders data of this model]

public:
    Fsphere sphere; //
    Fbox box; //
    u32 marker[R__NUM_CONTEXTS]; // for different sub-renders
    u32 valuable_frame[R__NUM_CONTEXTS]; // when the static geometry filter last kept it - hysteresis
    // The main view's fade across its draw distance (r__dsgraph_build.cpp): the frame it was last judged, the time
    // its verdict began, the verdict, and the share of its pixels the frame draws (the walk's product).
    u32 fade_frame;
    float fade_since;
    float fade_draw;
    bool fade_in;
    u32 accept_frame; // when it was requisted accepted for main render
    u32 hom_frame; // when to perform test - shedule
    u32 hom_tested; // when it was last time tested

    vis_object_data* obj_data; //--#SM+#-- Объектные данные, используемые при рендере этой модели [shaders data which
                               //will be used at render for this model]

    vis_data() //--#SM+#--
    {
        obj_data = &obj_data_self;
    }

    IC void clear()
    {
        sphere.P.set(0, 0, 0);
        sphere.R = 0;
        box.invalidate();
        memset(marker, 0, sizeof(marker));
        memset(valuable_frame, 0, sizeof(valuable_frame));
        fade_frame = 0;
        fade_since = 0.f;
        fade_draw = 1.f;
        fade_in = false;
        accept_frame = 0;
        hom_frame = 0;
        hom_tested = 0;
    }
};
#pragma pack(pop)
