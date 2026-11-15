// The module data reader (src/xrCore/XMS/xms_luadata.cpp) without the game: tools\qa\luadata\Run-LuaDataQa.ps1.
// Characters and the texture, sound and model descriptions are read by it.

#include "xrCore/XMS/xms_luadata.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
int failures = 0;

void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

bool read(const char* text, XMS::LuaValue& out, std::string& error)
{
    return XMS::ReadLuaData(text, std::strlen(text), out, error);
}

bool refused(const char* text, const char* line)
{
    XMS::LuaValue v;
    std::string error;
    return !read(text, v, error) && error.rfind(line, 0) == 0;
}

void lift_name(XMS::LuaValue& asset)
{
    XMS::LuaValue name;
    name.type = XMS::LuaValue::String;
    name.s = "lifted";
    asset.set("name", static_cast<XMS::LuaValue&&>(name));
}
} // namespace

int main()
{
    {
        XMS::LuaValue v;
        std::string error;
        const bool ok = read("-- a texture\nreturn {\n  tex = 1, name = 'a\\\\b', bump = { parallax = true },\n"
                             "  detail = { texture = [[d]], scale = -2.5e1 }, [\"x y\"] = false,\n"
                             "  surfaces = { { texture = \"t\" }, {}, }; [3] = 7,\n}\n",
            v, error);
        expect(ok && v.type == XMS::LuaValue::Table, "a description reads as one table");
        expect(v.number("tex", 0) == 1 && v.text("name") == "a\\b", "numbers and escaped strings");
        expect(v.get("bump") && v.get("bump")->flag("parallax", false), "a nested table and a boolean");
        expect(v.get("detail") && v.get("detail")->text("texture") == "d" &&
                v.get("detail")->number("scale", 0) == -25.0,
            "a long string and a negative exponent");
        expect(v.get("x y") && v.get("x y")->type == XMS::LuaValue::Bool, "a bracketed string key");
        expect(v.get("surfaces") && v.get("surfaces")->arr.size() == 2, "a list with a trailing separator");
        expect(v.get("3") && v.get("3")->n == 7, "a number key that is not the next index");
    }

    expect(refused("return { a = 1 }\nreturn 2", "line 2: "), "two values are refused, on their line");
    expect(refused("return {\n  a = os.exit(),\n}", "line 2: "), "a call is refused, on its line");
    expect(refused("return {\n\n  a = 1 + 2 }", "line 3: "), "an expression is refused, on its line");
    expect(refused("return { a = function() end }", "line 1: "), "a function is refused");
    expect(refused("return { a = 'open", "line 1: "), "an open string is refused");
    expect(refused("return {", "line 1: "), "an open table is refused");

    {
        XMS::LuaValue v;
        std::string error, why;
        read("return { tex = 2 }", v, error);
        expect(!XMS::UpgradeLuaData(v, "tex", 1, nullptr, true, why) &&
                why == "made for a newer game (version 2, this game reads up to 1)",
            "a newer version is refused by name");
        read("return { name = 'x' }", v, error);
        expect(!XMS::UpgradeLuaData(v, "tex", 1, nullptr, true, why) && why == "the header 'tex = 1' is missing",
            "a description without its header is refused");
        expect(XMS::UpgradeLuaData(v, "npc", 1, nullptr, false, why), "a character without its header is version 1");
        read("return { tex = 'one' }", v, error);
        expect(!XMS::UpgradeLuaData(v, "tex", 1, nullptr, true, why), "a header that is no version is refused");
        read("return { tex = 1 }", v, error);
        const XMS::LuaDataStep steps[] = {lift_name};
        expect(XMS::UpgradeLuaData(v, "tex", 2, steps, true, why) && v.text("name") == "lifted" &&
                v.number("tex", 0) == 2,
            "an older version goes through its steps");
        read("return { tex = 2 }", v, error);
        expect(XMS::UpgradeLuaData(v, "tex", 2, steps, true, why) && v.text("name").empty(),
            "the current version takes no step");
    }

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
