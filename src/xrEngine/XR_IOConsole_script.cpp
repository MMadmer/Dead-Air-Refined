#include "stdafx.h"

#include "XR_IOConsole.h"
#include "Render.h"
#include "xr_ioc_cmd.h"

#include "xrScriptEngine/script_space.hpp"
#include "xrCore/XMS/xms_catalog.h"

void CConsole::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        def("get_console", +[]()
        {
            return Console;
        }),

        class_<CConsole>("CConsole")
            .def("execute", +[](CConsole* self, pcstr command)
            {
                // a script's command is a script's even inside one the player typed (run_string);
                // the console checks it in protected mode (MOD_CATALOG 9.2)
                XMS::Protect::OriginScope scope(XMS::Protect::Origin::Script);
                self->Execute(command);
            })
            .def("execute_script", +[](CConsole* self, pcstr script)
            {
                // every line of the file is checked in turn: it runs under the script's origin
                XMS::Protect::OriginScope scope(XMS::Protect::Origin::Script);
                self->ExecuteScript(script);
            })
            .def("show", &CConsole::Show)
            .def("hide", &CConsole::Hide)

            .def("get_string", &CConsole::GetString)
            .def("get_integer", +[](const CConsole* self, pcstr cmd)
            {
                int min = 0, max = 0;
                const int val = self->GetInteger(cmd, min, max);
                return val;
            })
            .def("get_bool", +[](const CConsole* self, pcstr cmd)
            {
                return self->GetBool(cmd);
            })
            .def("get_float", +[](const CConsole* self, pcstr cmd)
            {
                float min = 0.0f, max = 0.0f;
                const float val = self->GetFloat(cmd, min, max);
                return val;
            })
            .def("get_token", &CConsole::GetToken)
            .def("execute_deferred", +[](CConsole*, pcstr string_to_execute)
            {
                Engine.Event.Defer("KERNEL:console", size_t(xr_strdup(string_to_execute)));
            }),

        def("renderer_allow_override", +[]()
        {
            return renderer_allow_override;
        }),

        def("level_texture_unload_supported", +[]()
        {
            return GEnv.Render && GEnv.Render->GetBackendAPI() == IRender::BackendAPI::D3D11;
        })
    ];
}
