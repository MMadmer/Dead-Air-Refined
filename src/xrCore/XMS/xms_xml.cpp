#include "stdafx.h"

#include "xms_xml.h"
#include "xms_core.h"
#include "xrCore/XML/XMLDocument.hpp"

#include <io.h>

#include "xrCommon/xr_unordered_map.h"
#include "xrCommon/xr_string.h"

namespace XMS
{
namespace
{
struct PatchRef
{
    xr_string module_id;
    u16 layer{0};
    TiXmlElement* patch_root{nullptr}; // <xms-patch> element, owned by doc below
    std::shared_ptr<TiXmlDocument> doc;
};

struct XmlState
{
    bool built{false};
    xr_unordered_map<xr_string, xr_vector<PatchRef>> by_target;
    // last module that touched a node, for ledger attribution
    xr_unordered_map<xr_string, xr_string> last_toucher;
};

XmlState& xs()
{
    static XmlState state;
    return state;
}

xr_string normalize_target(pcstr name)
{
    xr_string result(name ? name : "");
    for (char& c : result)
    {
        if (c == '/')
            c = '\\';
        else
            c = char(tolower(u8(c)));
    }
    return result;
}

void collect_xmlp(pcstr dir, xr_vector<xr_string>& out)
{
    string_path pattern;
    strconcat(pattern, dir, "*");
    _finddata_t entry;
    const intptr_t handle = _findfirst(pattern, &entry);
    if (handle == -1)
        return;
    do
    {
        if (0 == xr_strcmp(entry.name, ".") || 0 == xr_strcmp(entry.name, ".."))
            continue;
        string_path full;
        strconcat(full, dir, entry.name);
        if (entry.attrib & _A_SUBDIR)
        {
            xr_strcat(full, DELIMITER);
            collect_xmlp(full, out);
            continue;
        }
        const size_t len = xr_strlen(entry.name);
        if (len > 5 && 0 == _stricmp(entry.name + len - 5, ".xmlp"))
            out.emplace_back(full);
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
    std::sort(out.begin(), out.end());
}

void index_patch_file(XmlState& s, pcstr file, pcstr owner_id, u16 layer)
{
    FILE* f = fopen(file, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    xr_string text;
    text.resize(size_t(std::max(0l, len)));
    const size_t got = len > 0 ? fread(text.data(), 1, size_t(len), f) : 0;
    fclose(f);
    text.resize(got);
    if (text.empty())
        return;

    auto doc = std::make_shared<TiXmlDocument>();
    doc->Parse(doc.get(), text.c_str());
    if (doc->Error())
    {
        Msg("! XMS: bad xmlp [%s]: %s", file, doc->ErrorDesc());
        return;
    }
    for (TiXmlElement* patch = doc->FirstChildElement("xms-patch"); patch;
         patch = patch->NextSiblingElement("xms-patch"))
    {
        pcstr target = patch->Attribute("target");
        if (!target || !target[0])
        {
            Msg("! XMS: xmlp without target attribute [%s]", file);
            continue;
        }
        PatchRef ref;
        ref.module_id = owner_id;
        ref.layer = layer;
        ref.patch_root = patch;
        ref.doc = doc;
        s.by_target[normalize_target(target)].emplace_back(std::move(ref));
    }
}

void build_index()
{
    XmlState& s = xs();
    s.built = true;

    // The BASE layer: the game's own compatibility patches under
    // gamedata\patch\. Same format, applied before any module - this is how
    // the shipped game adjusts a screen it does not own (the UI xml belongs
    // to a third-party mod) without forking the whole file, and modules still
    // patch on top of the result.
    {
        string_path base_dir;
        FS.update_path(base_dir, "$game_data$", "patch" DELIMITER);
        xr_vector<xr_string> files;
        collect_xmlp(base_dir, files);
        for (const xr_string& file : files)
            index_patch_file(s, file.c_str(), "base", 0);
    }

    for (const Module& m : Modules())
    {
        if (m.disabled)
            continue;
        xr_vector<xr_string> files;
        string_path dir;
        strconcat(dir, m.root.c_str(), "patch" DELIMITER);
        collect_xmlp(dir, files);
        for (const xr_string& file : files)
            index_patch_file(s, file.c_str(), m.id.c_str(), m.layer);
    }
    if (!s.by_target.empty())
        Msg("* XMS: xml patch index: %zu target file(s)", s.by_target.size());
}

// path segment: tag, tag#index, tag[attr=value]
TiXmlElement* find_child(TiXmlElement* parent, pcstr segment)
{
    string256 tag;
    xr_strcpy(tag, segment);
    int wanted_index = 0;
    string256 attr_name = "", attr_value = "";

    if (char* hash = strchr(tag, '#'))
    {
        wanted_index = atoi(hash + 1);
        *hash = 0;
    }
    else if (char* bracket = strchr(tag, '['))
    {
        *bracket = 0;
        char* spec = bracket + 1;
        if (char* close = strchr(spec, ']'))
            *close = 0;
        if (char* eq = strchr(spec, '='))
        {
            *eq = 0;
            xr_strcpy(attr_name, spec);
            xr_strcpy(attr_value, eq + 1);
        }
    }

    int index = 0;
    for (TiXmlElement* child = parent->FirstChildElement(tag); child; child = child->NextSiblingElement(tag))
    {
        if (attr_name[0])
        {
            pcstr value = child->Attribute(attr_name);
            if (!value || 0 != xr_strcmp(value, attr_value))
                continue;
            return child;
        }
        if (index++ == wanted_index)
            return child;
    }
    return nullptr;
}

TiXmlElement* resolve_path(TiXmlElement* root, pcstr path)
{
    if (!path || !path[0])
        return root;
    string512 buffer;
    xr_strcpy(buffer, path);
    TiXmlElement* node = root;
    for (char* cursor = buffer; cursor && *cursor && node;)
    {
        char* colon = strchr(cursor, ':');
        if (colon)
            *colon = 0;
        node = find_child(node, cursor);
        cursor = colon ? colon + 1 : nullptr;
    }
    return node;
}

void ledger_touch(pcstr target, pcstr node_path, const PatchRef& ref)
{
    XmlState& s = xs();
    xr_string key = xr_string(target) + ":" + (node_path ? node_path : "");
    const auto it = s.last_toucher.find(key);
    AddConflict(ConflictKind::XmlPatch, key.c_str(), it != s.last_toucher.end() ? it->second.c_str() : "base",
        ref.module_id.c_str());
    s.last_toucher[key] = ref.module_id;
}

u32 apply_patch(XMLDocument& doc, pcstr target, const PatchRef& ref)
{
    TiXmlElement* root = doc.GetRoot() ? doc.GetRoot()->ToElement() : nullptr;
    if (!root)
        return 0;
    u32 applied = 0;
    for (TiXmlElement* op = ref.patch_root->FirstChildElement(); op; op = op->NextSiblingElement())
    {
        pcstr op_name = op->Value();
        if (0 == xr_strcmp(op_name, "append"))
        {
            TiXmlElement* into = resolve_path(root, op->Attribute("into"));
            if (!into)
            {
                Msg("! XMS: xmlp [%s] append target not found: %s @ %s", ref.module_id.c_str(),
                    op->Attribute("into") ? op->Attribute("into") : "", target);
                continue;
            }
            for (const TiXmlNode* child = op->FirstChild(); child; child = child->NextSibling())
                into->XmsInsertEndChildClone(*child);
            ledger_touch(target, op->Attribute("into"), ref);
            ++applied;
        }
        else if (0 == xr_strcmp(op_name, "set-attr"))
        {
            TiXmlElement* node = resolve_path(root, op->Attribute("node"));
            pcstr name = op->Attribute("name");
            pcstr value = op->Attribute("value");
            if (!node || !name || !value)
            {
                Msg("! XMS: xmlp [%s] set-attr failed: %s @ %s", ref.module_id.c_str(),
                    op->Attribute("node") ? op->Attribute("node") : "", target);
                continue;
            }
            node->XmsSetAttribute(name, value);
            ledger_touch(target, op->Attribute("node"), ref);
            ++applied;
        }
        else if (0 == xr_strcmp(op_name, "replace"))
        {
            TiXmlElement* node = resolve_path(root, op->Attribute("node"));
            const TiXmlNode* replacement = op->FirstChild();
            if (!node || node == root || !replacement || !node->Parent())
            {
                Msg("! XMS: xmlp [%s] replace failed: %s @ %s", ref.module_id.c_str(),
                    op->Attribute("node") ? op->Attribute("node") : "", target);
                continue;
            }
            TiXmlNode* parent = node->Parent();
            parent->XmsInsertBeforeChildClone(node, *replacement);
            parent->XmsRemoveChild(node);
            ledger_touch(target, op->Attribute("node"), ref);
            ++applied;
        }
        else if (0 == xr_strcmp(op_name, "remove"))
        {
            TiXmlElement* node = resolve_path(root, op->Attribute("node"));
            if (!node || node == root || !node->Parent())
            {
                Msg("! XMS: xmlp [%s] remove target not found: %s @ %s", ref.module_id.c_str(),
                    op->Attribute("node") ? op->Attribute("node") : "", target);
                continue;
            }
            node->Parent()->XmsRemoveChild(node);
            ledger_touch(target, op->Attribute("node"), ref);
            ++applied;
        }
    }
    return applied;
}
} // namespace

void ApplyXmlPatches(XMLDocument& doc, pcstr xml_filename)
{
    if (!Active())
        return;
    XmlState& s = xs();
    if (!s.built)
        build_index();
    if (s.by_target.empty())
        return;

    const xr_string target = normalize_target(xml_filename);
    const auto it = s.by_target.find(target);
    if (it == s.by_target.end())
        return;

    u32 total = 0;
    for (const PatchRef& ref : it->second)
        total += apply_patch(doc, target.c_str(), ref);
    if (total)
        Msg("* XMS: %u xml patch op(s) applied to [%s]", total, xml_filename);
}
} // namespace XMS
