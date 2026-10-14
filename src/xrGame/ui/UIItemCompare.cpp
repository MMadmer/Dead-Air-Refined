// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "pch_script.h"
#include "UIItemCompare.h"

#include "UIXmlInit.h"
#include "UIActorMenuTools.h"
#include "UIInventoryUtilities.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/Cursor/UICursor.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Static/UIShape.h"
#include "xrUICore/Static/UIStatic.h"
#include "xrUICore/XML/xrUIXmlParser.h"
#include "xrUICore/ui_base.h"
#include "xrEngine/xr_level_controller.h"
#include "xrScriptEngine/script_engine.hpp"
#include "Include/xrRender/Kinematics.h"
#include <bit>

#include "Actor.h"
#include "ActorCondition.h"
#include "ActorBackpack.h"
#include "ActorHelmet.h"
#include "Artefact.h"
#include "CustomDetector.h"
#include "CustomOutfit.h"
#include "da_artefact_container.h"
#include "eatable_item.h"
#include "Grenade.h"
#include "GrenadeLauncher.h"
#include "inventory_item.h"
#include "Level.h"
#include "Scope.h"
#include "Silencer.h"
#include "Weapon.h"
#include "WeaponAmmo.h"
#include "WeaponBinoculars.h"
#include "WeaponKnife.h"

using namespace item_compare;

namespace
{
constexpr pcstr config_file = "dead_air_x64_item_compare.ltx";
constexpr pcstr config_mask = "item_compare_*.ltx";

constexpr pcstr kind_names[] = { "none", "firearm", "melee", "outfit", "helmet", "backpack", "artefact", "ammo",
    "grenade", "addon", "detector", "food", "medicine", "misc" };
static_assert(std::size(kind_names) == size_t(EKind::Count));

// What an engine source is when its line in the data says no more than the weights.
struct SSourceDefaults
{
    pcstr id;
    bool higher;
    pcstr format;
    float scale;
};
constexpr SSourceDefaults engine_sources[] = {
    { "condition", true, "pct", 1.0f },
    { "weight", false, "kg", 1.0f },
    { "price", true, "money", 1.0f },
    { "price_per_kg", true, "money", 1.0f },
    { "uses", true, "int", 1.0f },
    { "damage", true, "num", 100.0f },
    { "damage_heavy", true, "num", 100.0f },
    { "rpm", true, "int", 1.0f },
    { "accuracy", false, "deg", 1.0f },
    { "recoil", false, "deg", 1.0f },
    { "handling", false, "num", 1.0f },
    { "aim_time", false, "sec", 1.0f },
    { "mag_size", true, "int", 1.0f },
    { "bullet_speed", true, "mps", 1.0f },
    { "endurance", true, "int", 1.0f },
    { "misfire", false, "pct", 1.0f },
    { "fouled", false, "faults", 1.0f },
    { "deformed", false, "faults", 1.0f },
    { "broken", false, "faults", 1.0f },
    { "addons", true, "int", 1.0f },
    { "mounts", true, "int", 1.0f },
    { "upgrades", true, "int", 1.0f },
    { "ammo_cost", false, "money", 1.0f },
    { "armor", true, "pct", 1.0f },
    { "artefact_slots", true, "int", 1.0f },
    { "carry", true, "kg", 1.0f },
    { "power_loss", false, "num", 1.0f },
    { "night_vision", true, "yes", 1.0f },
    { "restore_health", true, "num", 100.0f },
    { "restore_power", true, "num", 100.0f },
    { "restore_bleeding", true, "num", 100.0f },
    { "restore_satiety", true, "num", 100.0f },
    { "restore_radiation", false, "num", 100.0f },
    { "rounds", true, "int", 1.0f },
    { "round_cost", false, "money", 1.0f },
};

constexpr std::pair<pcstr, ALife::EHitType> hit_types[] = { { "burn", ALife::eHitTypeBurn },
    { "shock", ALife::eHitTypeShock }, { "chemical", ALife::eHitTypeChemicalBurn },
    { "radiation", ALife::eHitTypeRadiation }, { "psi", ALife::eHitTypeTelepatic }, { "strike", ALife::eHitTypeStrike },
    { "wound", ALife::eHitTypeWound }, { "explosion", ALife::eHitTypeExplosion },
    { "fire_wound", ALife::eHitTypeFireWound } };

// A fault of a weapon: the part it hits and how bad its state is.
struct SFault
{
    float penalty{};
    u8 state{}; // 0 fouled, 1 worn, 2 broken
};

struct SConfig
{
    bool loaded{};
    u32 max_items{ 8 };
    // [kind][row]
    xr_vector<SParam> params[u32(EKind::Count)];
    SFault faults[32]{};
};

SConfig g_config;

pcstr param_default_format(const shared_str& source)
{
    for (const SSourceDefaults& d : engine_sources)
    {
        if (source == d.id)
            return d.format;
    }
    if (0 == strncmp(source.c_str(), "prot_", 5) || 0 == strncmp(source.c_str(), "imm_", 4))
        return "pct";
    return "num";
}

void apply_defaults(SParam& param)
{
    param.higher = true;
    param.scale = 1.0f;
    param.format = param_default_format(param.source);
    for (const SSourceDefaults& d : engine_sources)
    {
        if (param.source == d.id)
        {
            param.higher = d.higher;
            param.scale = d.scale;
            break;
        }
    }
    if (!param.caption.size())
    {
        string128 caption;
        xr_sprintf(caption, "st_cmp_p_%s", param.id.c_str());
        param.caption = caption;
    }
}

// <weight to keep>, <weight to sell> [| <source> | <better: high, low> | <caption> | <format> | <scale>]
void read_param(xr_vector<SParam>& params, pcstr id, pcstr line)
{
    const auto it = std::find_if(params.begin(), params.end(), [id](const SParam& p) { return p.id == id; });
    if (!line || 0 == xr_stricmp(line, "remove"))
    {
        if (it != params.end())
            params.erase(it);
        return;
    }

    SParam param;
    param.id = id;
    param.source = id;
    string256 part;
    const int parts = _GetItemCount(line, '|');
    _GetItem(line, 0, part, '|');
    string64 number;
    param.weight[Use] = float(atof(_GetItem(part, 0, number)));
    param.weight[Sale] = _GetItemCount(part) > 1 ? float(atof(_GetItem(part, 1, number))) : 0.0f;
    if (parts > 1 && _Trim(_GetItem(line, 1, part, '|'))[0])
        param.source = part;
    apply_defaults(param);
    if (parts > 2 && _Trim(_GetItem(line, 2, part, '|'))[0])
        param.higher = 0 != xr_stricmp(part, "low");
    if (parts > 3 && _Trim(_GetItem(line, 3, part, '|'))[0])
        param.caption = part;
    if (parts > 4 && _Trim(_GetItem(line, 4, part, '|'))[0])
        param.format = part;
    if (parts > 5 && _Trim(_GetItem(line, 5, part, '|'))[0])
        param.scale = float(atof(part));

    if (it != params.end())
        *it = param;
    else
        params.push_back(param);
}

void read_config(const CInifile& ini)
{
    if (ini.section_exist("item_compare"))
        g_config.max_items = std::max(2u, ini.read_if_exists<u32>("item_compare", "max_items", g_config.max_items));

    // The rows of a kind: those named by rows= in that order, then the rest of its lines.
    string128 section;
    for (u32 k = 1; k < u32(EKind::Count); ++k)
    {
        xr_sprintf(section, "item_compare_%s", kind_names[k]);
        if (!ini.section_exist(section))
            continue;
        xr_vector<SParam>& params = g_config.params[k];
        xr_vector<shared_str> order;
        if (pcstr rows = ini.read_if_exists<pcstr>(section, "rows", nullptr))
        {
            string128 id;
            for (int i = 0, n = _GetItemCount(rows); i < n; ++i)
                order.emplace_back(_Trim(_GetItem(rows, i, id)));
        }
        for (const auto& [name, value] : ini.r_section(section).Data)
        {
            if (name.size() && name != "rows")
                read_param(params, name.c_str(), value.c_str());
        }
        // the named rows first, in their order; the rest after them as they were
        size_t placed = 0;
        for (const shared_str& id : order)
        {
            const auto it = std::find_if(params.begin() + placed, params.end(), [&id](const SParam& p) { return p.id == id; });
            if (it != params.end())
                std::rotate(params.begin() + placed++, it, it + 1);
        }
    }

    // A weapon's faults: the part's importance times the state's badness.
    if (ini.section_exist("item_compare_faults"))
    {
        for (const auto& [name, value] : ini.r_section("item_compare_faults").Data)
        {
            const int id = atoi(name.c_str());
            if (id < 1 || id > 32 || !value.size())
                continue;
            string64 part, state;
            _Trim(_GetItem(value.c_str(), 0, part));
            _Trim(_GetItem(value.c_str(), 1, state));
            const float importance = ini.read_if_exists<float>("item_compare_parts", part, 0.5f);
            const float badness = ini.read_if_exists<float>("item_compare_fault_states", state, 0.5f);
            SFault& fault = g_config.faults[id - 1];
            fault.penalty = importance * badness;
            fault.state = 0 == xr_stricmp(state, "fouled") ? 0 : (0 == xr_stricmp(state, "broken") ? 2 : 1);
        }
    }
}

void load()
{
    g_config = {};
    g_config.loaded = true;

    string_path path;
    FS.update_path(path, "$game_config$", config_file);
    if (FS.exist(path))
        read_config(CInifile(path, TRUE));

    FS_FileSet files;
    FS.file_list(files, "$game_config$", FS_ListFiles | FS_RootOnly, config_mask);
    for (const auto& file : files)
    {
        FS.update_path(path, "$game_config$", file.name.c_str());
        read_config(CInifile(path, TRUE));
    }
}

const SConfig& config()
{
    if (!g_config.loaded)
        load();
    return g_config;
}

float zone_max(ALife::EHitType type)
{
    const CActor* actor = Actor();
    const float max = actor ? actor->conditions().GetZoneMaxPower(type) : 1.0f;
    return max > EPS ? max : 1.0f;
}

float bone_armor_fraction(float armor)
{
    const CActor* actor = Actor();
    const float max = actor ? actor->conditions().GetMaxFireWoundProtection() : 1.0f;
    return max > EPS ? armor / max : armor;
}

u16 actor_bone(pcstr name)
{
    const CActor* actor = Actor();
    IKinematics* kinematics = actor ? smart_cast<IKinematics*>(const_cast<CActor*>(actor)->Visual()) : nullptr;
    return kinematics ? kinematics->LL_BoneID(name) : BI_NONE;
}

bool hit_type_of(pcstr name, ALife::EHitType& type)
{
    for (const auto& [id, hit] : hit_types)
    {
        if (0 == xr_strcmp(name, id))
        {
            type = hit;
            return true;
        }
    }
    return false;
}

float restore_of(CInventoryItem& item, ALife::EConditionRestoreType type, bool& known)
{
    known = true;
    const auto pick = [type](float health, float power, float bleeding, float satiety, float radiation)
    {
        switch (type)
        {
        case ALife::eHealthRestoreSpeed: return health;
        case ALife::ePowerRestoreSpeed: return power;
        case ALife::eBleedingRestoreSpeed: return bleeding;
        case ALife::eSatietyRestoreSpeed: return satiety;
        default: return radiation;
        }
    };
    if (CArtefact* af = smart_cast<CArtefact*>(&item))
    {
        return pick(af->GetHealthPower(), af->GetPowerPower(), af->GetBleedingPower(), af->GetSatietyPower(),
                   af->GetRadiationPower()) * item.GetCondition();
    }
    if (CCustomOutfit* outfit = smart_cast<CCustomOutfit*>(&item))
    {
        return pick(outfit->m_fHealthRestoreSpeed, outfit->m_fPowerRestoreSpeed, outfit->m_fBleedingRestoreSpeed,
            outfit->m_fSatietyRestoreSpeed, outfit->m_fRadiationRestoreSpeed);
    }
    if (CHelmet* helmet = smart_cast<CHelmet*>(&item))
    {
        return pick(helmet->m_fHealthRestoreSpeed, helmet->m_fPowerRestoreSpeed, helmet->m_fBleedingRestoreSpeed,
            helmet->m_fSatietyRestoreSpeed, helmet->m_fRadiationRestoreSpeed);
    }
    if (CBackpack* backpack = smart_cast<CBackpack*>(&item))
        return type == ALife::ePowerRestoreSpeed ? backpack->m_fPowerRestoreSpeed : 0.0f;
    known = false;
    return 0.0f;
}

// The value of one parameter of one item; `shown` is what its cell prints.
bool read_value(const SParam& param, CInventoryItem& item, const price_function& price, float& value, float& shown)
{
    const shared_str& source = param.source;
    pcstr id = source.c_str();
    CWeapon* weapon = smart_cast<CWeapon*>(&item);
    bool known = true;

    const auto weapon_stats = [weapon]() { return weapon->CompareStats(); };

    if (0 == strncmp(id, "key:", 4))
    {
        value = pSettings->read_if_exists<float>(item.object().cNameSect(), id + 4, 0.0f);
    }
    else if (0 == strncmp(id, "script:", 7))
    {
        luabind::functor<float> function;
        if (!GEnv.ScriptEngine->functor(id + 7, function))
            return false;
        // a function that returns nothing for an item says it has no such value
        try
        {
            value = function(item.object().lua_game_object());
        }
        catch (...)
        {
            return false;
        }
    }
    else if (source == "condition")
        value = item.GetCondition();
    else if (source == "weight")
        value = item.Weight();
    else if (source == "price")
        value = float(price(item));
    else if (source == "price_per_kg")
        value = float(price(item)) / std::max(item.Weight(), 0.05f);
    else if (source == "uses")
    {
        CEatableItem* eatable = item.cast_eatable_item();
        value = eatable && eatable->GetRemainingUses() != u8(-1) ? float(eatable->GetRemainingUses()) : 1.0f;
    }
    else if (weapon && (source == "damage" || source == "damage_heavy" || source == "rpm" || source == "accuracy" ||
                 source == "recoil" || source == "aim_time" || source == "mag_size" || source == "bullet_speed" ||
                 source == "endurance"))
    {
        const CWeapon::SCompareStats stats = weapon_stats();
        if (source == "damage")
            value = stats.damage;
        else if (source == "damage_heavy")
            value = stats.damage_heavy;
        else if (source == "rpm")
            value = stats.rpm;
        else if (source == "accuracy")
            value = stats.dispersion;
        else if (source == "recoil")
            value = stats.recoil;
        else if (source == "aim_time")
            value = stats.aim_time;
        else if (source == "mag_size")
            value = float(stats.magazine);
        else if (source == "bullet_speed")
            value = stats.bullet_speed;
        else
            value = 0.1f / std::max(stats.wear, 1e-6f); // shots per tenth of the condition
        known = value > 0.0f || source == "mag_size";
    }
    else if (weapon && source == "handling")
        value = weapon->GetControlInertionFactor();
    else if (weapon && source == "misfire")
        value = weapon->GetConditionMisfireProbability();
    else if (weapon && (source == "fouled" || source == "deformed" || source == "broken"))
    {
        const u8 state = source == "fouled" ? 0 : (source == "deformed" ? 1 : 2);
        const u32 faults = weapon->GetConditionType() & kWeaponDisplayableFaultMask;
        u32 count = 0;
        value = 0.0f;
        for (u32 bits = faults; bits; bits &= bits - 1)
        {
            const SFault& fault = config().faults[std::countr_zero(bits)];
            if (fault.state != state)
                continue;
            value += fault.penalty;
            ++count;
        }
        shown = float(count);
        return true;
    }
    else if (weapon && (source == "addons" || source == "mounts"))
    {
        const bool attached[] = { weapon->IsScopeAttached(), weapon->IsSilencerAttached(), weapon->IsGrenadeLauncherAttached() };
        const ALife::EWeaponAddonStatus status[] = { weapon->get_ScopeStatus(), weapon->get_SilencerStatus(),
            weapon->get_GrenadeLauncherStatus() };
        value = 0.0f;
        for (u32 i = 0; i < 3; ++i)
        {
            if (source == "addons")
                value += attached[i] && status[i] == ALife::eAddonAttachable ? 1.0f : 0.0f;
            else
                value += status[i] != ALife::eAddonDisabled ? 1.0f : 0.0f;
        }
    }
    else if (source == "upgrades")
        value = float(item.upgardes().size());
    else if (weapon && source == "ammo_cost")
    {
        // what a round of the weapon's first ammunition costs
        cpcstr ammo = !weapon->m_ammoTypes.empty() ? weapon->m_ammoTypes[0].c_str() : nullptr;
        if (!ammo)
            return false;
        const float box = std::max(1.0f, pSettings->read_if_exists<float>(ammo, "box_size", 1.0f));
        value = pSettings->read_if_exists<float>(ammo, "cost", 0.0f) / box;
    }
    else if (0 == strncmp(id, "prot_", 5) || source == "armor")
    {
        ALife::EHitType type = ALife::eHitTypeFireWound;
        if (source != "armor" && !hit_type_of(id + 5, type))
            return false;
        CCustomOutfit* outfit = smart_cast<CCustomOutfit*>(&item);
        CHelmet* helmet = smart_cast<CHelmet*>(&item);
        if (!outfit && !helmet)
            return false;
        if (type == ALife::eHitTypeFireWound)
        {
            // the armour class of the chest or the head, as the hint shows it
            const u16 bone = actor_bone(outfit ? "bip01_spine" : "bip01_head");
            if (bone == BI_NONE)
                return false;
            value = bone_armor_fraction((outfit ? outfit->GetBoneArmor(bone) : helmet->GetBoneArmor(bone)) * item.GetCondition());
        }
        else
            value = (outfit ? outfit->GetDefHitTypeProtection(type) : helmet->GetDefHitTypeProtection(type)) / zone_max(type);
    }
    else if (0 == strncmp(id, "imm_", 4))
    {
        ALife::EHitType type;
        CArtefact* af = smart_cast<CArtefact*>(&item);
        if (!af || !hit_type_of(id + 4, type))
            return false;
        value = af->ArtefactHitImmunity(type) * item.GetCondition() / zone_max(type);
    }
    else if (source == "artefact_slots")
    {
        CCustomOutfit* outfit = smart_cast<CCustomOutfit*>(&item);
        if (!outfit)
            return false;
        value = float(outfit->get_artefact_count());
    }
    else if (source == "carry")
    {
        if (CCustomOutfit* outfit = smart_cast<CCustomOutfit*>(&item))
            value = outfit->m_additional_weight;
        else if (CBackpack* backpack = smart_cast<CBackpack*>(&item))
            value = backpack->m_additional_weight;
        else if (CArtefact* af = smart_cast<CArtefact*>(&item))
            value = af->AdditionalInventoryWeight() * item.GetCondition();
        else
            return false;
    }
    else if (source == "power_loss")
    {
        if (CCustomOutfit* outfit = smart_cast<CCustomOutfit*>(&item))
            value = outfit->GetPowerLoss();
        else if (CHelmet* helmet = smart_cast<CHelmet*>(&item))
            value = helmet->m_fPowerLoss;
        else if (CBackpack* backpack = smart_cast<CBackpack*>(&item))
            value = backpack->m_fPowerLoss;
        else
            return false;
    }
    else if (source == "night_vision")
    {
        if (CCustomOutfit* outfit = smart_cast<CCustomOutfit*>(&item))
            value = outfit->m_NightVisionSect.size() ? 1.0f : 0.0f;
        else if (CHelmet* helmet = smart_cast<CHelmet*>(&item))
            value = helmet->m_NightVisionSect.size() ? 1.0f : 0.0f;
        else
            return false;
    }
    else if (0 == strncmp(id, "restore_", 8))
    {
        constexpr std::pair<pcstr, ALife::EConditionRestoreType> restores[] = { { "health", ALife::eHealthRestoreSpeed },
            { "power", ALife::ePowerRestoreSpeed }, { "bleeding", ALife::eBleedingRestoreSpeed },
            { "satiety", ALife::eSatietyRestoreSpeed }, { "radiation", ALife::eRadiationRestoreSpeed } };
        const auto it = std::find_if(std::begin(restores), std::end(restores), [id](const auto& r) { return 0 == xr_strcmp(id + 8, r.first); });
        if (it == std::end(restores))
            return false;
        value = restore_of(item, it->second, known);
    }
    else if (source == "rounds" || source == "round_cost")
    {
        CWeaponAmmo* ammo = smart_cast<CWeaponAmmo*>(&item);
        if (!ammo)
            return false;
        if (source == "rounds")
            value = float(ammo->m_boxCurr);
        else
            value = pSettings->read_if_exists<float>(item.object().cNameSect(), "cost", 0.0f) / std::max<float>(1.0f, ammo->m_boxSize);
    }
    else
        return false;

    shown = value;
    return known;
}
} // namespace

EKind item_compare::kind_of(CInventoryItem& item)
{
    if (CWeapon* weapon = smart_cast<CWeapon*>(&item))
    {
        if (smart_cast<CWeaponKnife*>(weapon))
            return EKind::Melee;
        return smart_cast<CWeaponBinoculars*>(weapon) ? EKind::Detector : EKind::Firearm;
    }
    if (smart_cast<CCustomOutfit*>(&item))
        return EKind::Outfit;
    if (smart_cast<CHelmet*>(&item))
        return EKind::Helmet;
    if (smart_cast<CBackpack*>(&item))
        return EKind::Backpack;
    if (smart_cast<CArtefact*>(&item))
        return da_af_container::is_container(item.object().cNameSect().c_str()) ? EKind::Misc : EKind::Artefact;
    if (smart_cast<CWeaponAmmo*>(&item))
        return EKind::Ammo;
    if (smart_cast<CGrenade*>(&item))
        return EKind::Grenade;
    if (smart_cast<CScope*>(&item) || smart_cast<CSilencer*>(&item) || smart_cast<CGrenadeLauncher*>(&item))
        return EKind::Addon;
    if (smart_cast<CCustomDetector*>(&item))
        return EKind::Detector;
    switch (inventory_tools::classify(item))
    {
    case EItemCategory::Food: return EKind::Food;
    case EItemCategory::Medicine: return EKind::Medicine;
    case EItemCategory::Weapons: return EKind::Addon; // an addon of a class of a script's own
    default: return EKind::Misc;
    }
}

pcstr item_compare::kind_caption(EKind kind)
{
    static string64 captions[u32(EKind::Count)];
    string64& caption = captions[u32(kind)];
    if (!caption[0])
        xr_sprintf(caption, "st_cmp_kind_%s", kind_names[u32(kind)]);
    return caption;
}

u32 item_compare::max_items() { return config().max_items; }

void item_compare::reset() { g_config.loaded = false; }

SResult item_compare::evaluate(const xr_vector<CInventoryItem*>& items, EProfile profile, const price_function& price)
{
    SResult result;
    result.profile = profile;
    result.items = items;
    if (items.empty())
        return result;
    result.kind = kind_of(*items.front());

    const size_t n = items.size();
    for (const SParam& param : config().params[u32(result.kind)])
    {
        xr_vector<SCell> row(n);
        bool any = false, nonzero = false;
        for (size_t i = 0; i < n; ++i)
        {
            SCell& cell = row[i];
            cell.known = read_value(param, *items[i], price, cell.value, cell.shown) && std::isfinite(cell.value);
            any |= cell.known;
            nonzero |= cell.known && !fis_zero(cell.value, 1e-7f);
        }
        // a row nobody has says nothing
        if (!any || !nonzero)
            continue;

        // Each value against the best of the row, on the scale of the biggest magnitude: a ratio
        // for positive values, still sound for the signed ones (an artefact's radiation).
        float lo = flt_max, hi = -flt_max;
        for (const SCell& cell : row)
        {
            if (cell.known)
            {
                lo = std::min(lo, cell.value);
                hi = std::max(hi, cell.value);
            }
        }
        const float best = param.higher ? hi : lo;
        const float ref = std::max(std::abs(hi), std::abs(lo));
        for (SCell& cell : row)
            cell.norm = !cell.known ? 0.0f : (ref < 1e-9f ? 1.0f : clampr(1.0f - std::abs(best - cell.value) / ref, 0.0f, 1.0f));

        result.rows.push_back(&param);
        result.cells.push_back(std::move(row));
    }

    result.score.assign(n, 0.0f);
    for (size_t r = 0; r < result.rows.size(); ++r)
    {
        const float weight = result.rows[r]->weight[profile];
        if (weight <= 0.0f)
            continue;
        result.weight_sum += weight;
        for (size_t i = 0; i < n; ++i)
            result.score[i] += weight * result.cells[r][i].norm;
    }
    if (result.weight_sum > 0.0f)
    {
        for (float& score : result.score)
            score = score * 100.0f / result.weight_sum;
        result.winner = int(std::max_element(result.score.begin(), result.score.end()) - result.score.begin());

        // Why it won: the rows it gained most on against the average of the others.
        if (n > 1)
        {
            xr_vector<std::pair<float, const SParam*>> gains;
            for (size_t r = 0; r < result.rows.size(); ++r)
            {
                const float weight = result.rows[r]->weight[profile];
                float others = 0.0f;
                for (size_t i = 0; i < n; ++i)
                    others += i == size_t(result.winner) ? 0.0f : result.cells[r][i].norm;
                const float gain = weight * (result.cells[r][result.winner].norm - others / float(n - 1));
                if (gain > 0.01f)
                    gains.emplace_back(gain, result.rows[r]);
            }
            std::sort(gains.begin(), gains.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (size_t i = 0; i < gains.size() && i < 3; ++i)
                result.reasons.push_back(gains[i].second);
        }
    }
    return result;
}

xr_string item_compare::format_value(const SParam& param, float shown)
{
    const float v = shown * param.scale;
    string64 text;
    const shared_str& f = param.format;
    if (f == "int" || f == "faults")
        xr_sprintf(text, "%d", iFloor(v + 0.5f));
    else if (f == "pct")
        xr_sprintf(text, "%.0f%%", v * 100.0f);
    else if (f == "kg")
        xr_sprintf(text, "%.2f %s", v, StringTable().translate("st_cmp_unit_kg").c_str());
    else if (f == "money")
        xr_sprintf(text, v < 10.0f ? "%.1f %s" : "%.0f %s", v, StringTable().GetCurrency().c_str());
    else if (f == "sec")
        xr_sprintf(text, "%.2f %s", v, StringTable().translate("st_cmp_unit_sec").c_str());
    else if (f == "mps")
        xr_sprintf(text, "%.0f %s", v, StringTable().translate("st_cmp_unit_mps").c_str());
    else if (f == "deg")
        xr_sprintf(text, "%.2f\xB0", rad2deg(v)); // the degree sign of windows-1251, the fonts' own
    else if (f == "yes")
        xr_strcpy(text, StringTable().translate(v > 0.5f ? "st_cmp_yes" : "st_cmp_no").c_str());
    else
    {
        // as many decimals as the size needs
        const float a = std::abs(v);
        xr_sprintf(text, a >= 100.0f ? "%.0f" : (a >= 10.0f ? "%.1f" : (a >= 0.1f ? "%.2f" : "%.3f")), v);
    }
    return text;
}

// =================================================================================================

namespace
{
constexpr pcstr window_xml = "actor_menu_compare.xml";
constexpr pcstr window_node = "compare_window";
} // namespace

CUIItemCompareWnd::CUIItemCompareWnd() : CUIWindow("compare_window") {}
CUIItemCompareWnd::~CUIItemCompareWnd() = default;

bool CUIItemCompareWnd::Init(CUIXml& actor_menu_xml)
{
    if (actor_menu_xml.NavigateToNode(window_node, 0))
        return InitFrom(actor_menu_xml);
    CUIXml own;
    return own.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, window_xml, false) && own.NavigateToNode(window_node, 0) &&
        InitFrom(own);
}

void CUIItemCompareWnd::ReadText(CUIXml& xml, pcstr node, SText& text)
{
    string256 path;
    CUIXmlInit::InitFont(xml, strconcat(path, window_node, ":", node, ":text"), 0, text.color, text.font);
}

bool CUIItemCompareWnd::InitFrom(CUIXml& xml)
{
    CUIXmlInit::InitWindow(xml, window_node, 0, this);
    // separators and plates of the layout's own
    CUIXmlInit::InitAutoStaticGroup(xml, window_node, 0, this);
    m_row_height = xml.ReadAttribFlt(window_node, 0, "row_height", m_row_height);
    m_label_width = xml.ReadAttribFlt(window_node, 0, "label_width", m_label_width);
    m_column_width = xml.ReadAttribFlt(window_node, 0, "column_width", m_column_width);
    m_icon_height = xml.ReadAttribFlt(window_node, 0, "icon_height", m_icon_height);
    m_padding = xml.ReadAttribFlt(window_node, 0, "padding", m_padding);

    string256 path;
    const auto node = [&path](pcstr child) { return strconcat(path, window_node, ":", child); };

    if (const auto shade = UIShape::ReadSkin(xml, node("shade"), 0))
        m_shade = shade->looks[SUIShapeSkin::Normal];

    m_title = xr_new<CUIStatic>("title");
    m_title->SetAutoDelete(true);
    AttachChild(m_title);
    CUIXmlInit::InitStatic(xml, node("title"), 0, m_title);

    constexpr pcstr tabs[] = { "tab_use", "tab_sale" };
    for (u8 i = 0; i < ProfileCount; ++i)
    {
        m_tabs[i] = xr_new<CUI3tButton>();
        m_tabs[i]->SetAutoDelete(true);
        AttachChild(m_tabs[i]);
        CUIXmlInit::Init3tButton(xml, node(tabs[i]), 0, m_tabs[i]);
        m_tabs[i]->SetButtonAsSwitch(true);
        Register(m_tabs[i]);
        AddCallback(m_tabs[i], BUTTON_CLICKED, void_function(this, &CUIItemCompareWnd::OnTab));
    }

    if (xml.NavigateToNode(node("close"), 0))
    {
        m_close = xr_new<CUI3tButton>();
        m_close->SetAutoDelete(true);
        AttachChild(m_close);
        CUIXmlInit::Init3tButton(xml, path, 0, m_close);
        auto* cross = xr_new<CUIStatic>("cross");
        cross->SetAutoDelete(true);
        m_close->AttachChild(cross);
        const Fvector2 size = m_close->GetWndSize();
        const float side = size.y * 0.5f;
        const float kx = UI().get_current_kx();
        cross->SetWndPos({ (size.x - side * kx) * 0.5f, (size.y - side) * 0.5f });
        cross->SetWndSize({ side * kx, side });
        auto skin = std::make_unique<SUIShapeSkin>();
        skin->looks[SUIShapeSkin::Normal].kind = SUIShapeStyle::Kind::Cross;
        skin->looks[SUIShapeSkin::Normal].fill = UIShape::ParseColor(xml.ReadAttrib(path, 0, "cross", "$text_dim"), 0xFFFFFFFF);
        skin->looks[SUIShapeSkin::Normal].border = 1.5f;
        cross->SetShapeSkin(std::move(skin));
        cross->Enable(false);
        Register(m_close);
        AddCallback(m_close, BUTTON_CLICKED, void_function(this, &CUIItemCompareWnd::OnClose));
    }

    m_header = xr_new<CUIWindow>("columns");
    m_header->SetAutoDelete(true);
    AttachChild(m_header);
    CUIXmlInit::InitWindow(xml, node("columns"), 0, m_header);

    m_list = xr_new<CUIScrollView>();
    m_list->SetAutoDelete(true);
    AttachChild(m_list);
    CUIXmlInit::InitScrollView(xml, node("list"), 0, m_list);

    m_footer = xr_new<CUIStatic>("footer");
    m_footer->SetAutoDelete(true);
    AttachChild(m_footer);
    CUIXmlInit::InitStatic(xml, node("footer"), 0, m_footer);
    m_footer->SetTextComplexMode(true);

    ReadText(xml, "name", m_name);
    ReadText(xml, "score", m_score);
    ReadText(xml, "label", m_label);
    ReadText(xml, "weight", m_weight);
    ReadText(xml, "value", m_value);
    ReadText(xml, "section", m_section);
    m_best_color = UIShape::ParseColor(xml.ReadAttrib(node("best"), 0, "color", "$good"), m_best_color);
    m_worst_color = UIShape::ParseColor(xml.ReadAttrib(node("worst"), 0, "color", "$bad"), m_worst_color);
    m_column_skin = UIShape::ReadSkin(xml, node("column"), 0);
    m_winner_skin = UIShape::ReadSkin(xml, node("column_winner"), 0);
    m_row_skin = UIShape::ReadSkin(xml, node("row_alt"), 0);
    m_bar_skin = UIShape::ReadSkin(xml, node("score_bar"), 0);
    m_track_skin = UIShape::ReadSkin(xml, node("score_track"), 0);
    m_badge_skin = UIShape::ReadSkin(xml, node("badge"), 0);
    m_rule_skin = UIShape::ReadSkin(xml, node("rule"), 0);

    Show(false);
    return true;
}

void CUIItemCompareWnd::Open(const xr_vector<CInventoryItem*>& items, item_compare::price_function price)
{
    m_ids.clear();
    for (CInventoryItem* item : items)
        m_ids.push_back(item->object_id());
    m_price = std::move(price);
    Show(true);
    SetProfile(m_profile);
}

void CUIItemCompareWnd::Close()
{
    Show(false);
    m_ids.clear();
    m_header->DetachAll();
    m_list->Clear();
}

void CUIItemCompareWnd::SetProfile(EProfile profile)
{
    m_profile = profile;
    for (u8 i = 0; i < ProfileCount; ++i)
    {
        if (m_tabs[i])
            m_tabs[i]->SetButtonState(i == profile ? CUIButton::BUTTON_PUSHED : CUIButton::BUTTON_NORMAL);
    }
    Rebuild();
}

CUIStatic* CUIItemCompareWnd::AddText(CUIWindow* parent, pcstr text, const Frect& rect, const SText& look, u32 align, bool complex)
{
    auto* st = xr_new<CUIStatic>("text");
    st->SetAutoDelete(true);
    parent->AttachChild(st);
    st->SetWndRect(rect);
    if (look.font)
        st->TextItemControl()->SetFont(look.font);
    st->TextItemControl()->SetTextColor(look.color);
    st->TextItemControl()->SetTextAlignment(CGameFont::EAligment(align));
    st->TextItemControl()->SetVTextAlignment(valCenter);
    st->SetTextComplexMode(complex);
    st->SetText(text);
    st->Enable(false);
    return st;
}

CUIStatic* CUIItemCompareWnd::AddShape(CUIWindow* parent, const Frect& rect, const SUIShapeSkin* skin)
{
    if (!skin)
        return nullptr;
    auto* st = xr_new<CUIStatic>("shape");
    st->SetAutoDelete(true);
    parent->AttachChild(st);
    st->SetWndRect(rect);
    st->SetShapeSkin(std::make_unique<SUIShapeSkin>(*skin));
    st->Enable(false);
    return st;
}

CUIStatic* CUIItemCompareWnd::AddIcon(CUIWindow* parent, CInventoryItem& item, const Frect& box)
{
    const shared_str& section = item.object().cNameSect();
    Frect tex;
    tex.x1 = float(pSettings->r_u32(section, "inv_grid_x") * INV_GRID_WIDTH);
    tex.y1 = float(pSettings->r_u32(section, "inv_grid_y") * INV_GRID_HEIGHT);
    tex.x2 = tex.x1 + float(pSettings->r_u32(section, "inv_grid_width") * INV_GRID_WIDTH);
    tex.y2 = tex.y1 + float(pSettings->r_u32(section, "inv_grid_height") * INV_GRID_HEIGHT);

    // as big as the box allows, the icon's own proportions on screen
    const float kx = UI().get_current_kx();
    const float k = std::min(box.width() / (tex.width() * kx), box.height() / tex.height());
    const Fvector2 size{ tex.width() * kx * k, tex.height() * k };

    auto* icon = xr_new<CUIStatic>("icon");
    icon->SetAutoDelete(true);
    parent->AttachChild(icon);
    icon->SetShader(InventoryUtilities::GetItemIconsShader(section));
    icon->SetTextureRect(tex);
    icon->TextureOn();
    icon->SetStretchTexture(true);
    icon->SetWndPos({ box.x1 + (box.width() - size.x) * 0.5f, box.y1 + (box.height() - size.y) * 0.5f });
    icon->SetWndSize(size);
    icon->Enable(false);
    return icon;
}

void CUIItemCompareWnd::Rebuild()
{
    m_header->DetachAll();
    m_list->Clear();

    xr_vector<CInventoryItem*> items;
    for (const u16 id : m_ids)
    {
        if (CInventoryItem* item = smart_cast<CInventoryItem*>(Level().Objects.net_Find(id)))
            items.push_back(item);
    }
    if (items.size() < 2)
    {
        Close();
        return;
    }

    const SResult result = evaluate(items, m_profile, m_price);
    const size_t n = items.size();
    const float kx = UI().get_current_kx();
    const float pad = m_padding;

    // the title: what is compared and how many
    string256 title;
    xr_sprintf(title, StringTable().translate("st_cmp_title").c_str(), StringTable().translate(kind_caption(result.kind)).c_str(), u32(n));
    m_title->SetText(title);

    // Columns: as wide as the layout says while they fit, narrower when they do not.
    const float width = m_list->GetDesiredChildWidth();
    const float label = m_label_width * kx;
    const float column = std::min(m_column_width * kx, (width - label) / float(n));
    const auto column_x = [&](size_t i) { return label + column * float(i); };

    // The header: the item, its name, its score and the winner's star.
    const float header_h = m_header->GetHeight();
    AddText(m_header, StringTable().translate(m_profile == Use ? "st_cmp_profile_use_hint" : "st_cmp_profile_sale_hint").c_str(),
        Frect().set(pad * kx, 0.0f, label - pad * kx, header_h), m_label, CGameFont::alLeft, true);
    for (size_t i = 0; i < n; ++i)
    {
        const bool winner = int(i) == result.winner;
        const float x = column_x(i);
        AddShape(m_header, Frect().set(x + 2.0f * kx, 0.0f, x + column - 2.0f * kx, header_h), winner && m_winner_skin ? m_winner_skin.get() : m_column_skin.get());
        AddIcon(m_header, *items[i], Frect().set(x + pad * kx, pad, x + column - pad * kx, pad + m_icon_height));

        string256 name;
        xr_sprintf(name, "%u. %s", u32(i + 1), items[i]->NameItem());
        const float name_y = pad + m_icon_height + 2.0f;
        AddText(m_header, name, Frect().set(x + pad * kx, name_y, x + column - pad * kx, name_y + 30.0f), m_name, CGameFont::alCenter, true);

        const float score_y = header_h - pad - 26.0f;
        string32 score;
        xr_sprintf(score, "%.0f", result.score[i]);
        CUIStatic* score_text = AddText(m_header, score, Frect().set(x + pad * kx, score_y, x + column - pad * kx, score_y + 20.0f), m_score, CGameFont::alCenter);
        if (winner)
            score_text->TextItemControl()->SetTextColor(m_best_color);
        // the score as a bar under it
        const Frect track = Frect().set(x + pad * kx, header_h - pad - 4.0f, x + column - pad * kx, header_h - pad);
        AddShape(m_header, track, m_track_skin.get());
        AddShape(m_header, Frect().set(track.x1, track.y1, track.x1 + track.width() * result.score[i] / 100.0f, track.y2), m_bar_skin.get());
        if (winner && m_badge_skin)
        {
            const float side = 16.0f;
            AddShape(m_header, Frect().set(x + column - (pad + side) * kx, pad, x + column - pad * kx, pad + side), m_badge_skin.get());
        }
    }

    // A row: the caption, its weight, and the values with the best and the worst coloured.
    const auto add_row = [&](size_t index) -> CUIWindow*
    {
        auto* row = xr_new<CUIWindow>("row");
        row->SetWndSize({ width, m_row_height });
        if (index % 2 && m_row_skin)
            AddShape(row, Frect().set(0.0f, 0.0f, width, m_row_height), m_row_skin.get());
        m_list->AddWindow(row, true);
        return row;
    };

    for (size_t r = 0; r < result.rows.size(); ++r)
    {
        const SParam& param = *result.rows[r];
        const xr_vector<SCell>& cells = result.cells[r];
        CUIWindow* row = add_row(r);
        AddText(row, StringTable().translate(param.caption).c_str(), Frect().set(pad * kx, 0.0f, label - 40.0f * kx, m_row_height), m_label, CGameFont::alLeft);
        string32 weight;
        const float w = param.weight[m_profile];
        if (w > 0.0f && result.weight_sum > 0.0f)
            xr_sprintf(weight, "%.0f%%", w * 100.0f / result.weight_sum);
        else
            xr_strcpy(weight, "-");
        AddText(row, weight, Frect().set(label - 40.0f * kx, 0.0f, label - pad * kx, m_row_height), m_weight, CGameFont::alRight);

        // the best and the worst stand out only where the row tells them apart
        float lo = flt_max, hi = -flt_max;
        for (const SCell& cell : cells)
        {
            if (cell.known)
            {
                lo = std::min(lo, cell.norm);
                hi = std::max(hi, cell.norm);
            }
        }
        const bool differ = hi - lo > 0.005f;
        for (size_t i = 0; i < n; ++i)
        {
            const SCell& cell = cells[i];
            const xr_string text = cell.known ? format_value(param, cell.shown) : xr_string("-");
            CUIStatic* value = AddText(row, text.c_str(), Frect().set(column_x(i), 0.0f, column_x(i) + column, m_row_height), m_value, CGameFont::alCenter);
            if (differ && cell.known && w > 0.0f)
            {
                if (cell.norm >= hi - 0.005f)
                    value->TextItemControl()->SetTextColor(m_best_color);
                else if (cell.norm <= lo + 0.005f)
                    value->TextItemControl()->SetTextColor(m_worst_color);
            }
        }
    }

    // Everyone against everyone: the difference of the scores, row against column.
    {
        auto* gap = xr_new<CUIWindow>("gap");
        gap->SetWndSize({ width, m_row_height * 0.6f });
        if (m_rule_skin)
            AddShape(gap, Frect().set(pad * kx, m_row_height * 0.3f, width - pad * kx, m_row_height * 0.3f + 1.0f), m_rule_skin.get());
        m_list->AddWindow(gap, true);

        // the title across the whole width, the columns' numbers under it
        auto* title_row = xr_new<CUIWindow>("pairs_title");
        title_row->SetWndSize({ width, m_row_height });
        AddText(title_row, StringTable().translate("st_cmp_pairs").c_str(), Frect().set(pad * kx, 0.0f, width - pad * kx, m_row_height), m_section, CGameFont::alLeft);
        m_list->AddWindow(title_row, true);

        auto* head = xr_new<CUIWindow>("pairs");
        head->SetWndSize({ width, m_row_height });
        for (size_t i = 0; i < n; ++i)
        {
            string16 number;
            xr_sprintf(number, "%u", u32(i + 1));
            AddText(head, number, Frect().set(column_x(i), 0.0f, column_x(i) + column, m_row_height), m_section, CGameFont::alCenter);
        }
        m_list->AddWindow(head, true);

        for (size_t i = 0; i < n; ++i)
        {
            CUIWindow* row = add_row(i);
            string256 name;
            xr_sprintf(name, "%u. %s", u32(i + 1), items[i]->NameShort());
            AddText(row, name, Frect().set(pad * kx, 0.0f, label - pad * kx, m_row_height), m_label, CGameFont::alLeft);
            for (size_t j = 0; j < n; ++j)
            {
                string32 diff;
                const float d = result.score[i] - result.score[j];
                if (i == j)
                    xr_strcpy(diff, "-");
                else
                    xr_sprintf(diff, "%+.0f", d);
                CUIStatic* value = AddText(row, diff, Frect().set(column_x(j), 0.0f, column_x(j) + column, m_row_height), m_value, CGameFont::alCenter);
                if (i != j && std::abs(d) >= 0.5f)
                    value->TextItemControl()->SetTextColor(d > 0.0f ? m_best_color : m_worst_color);
            }
        }
    }

    // Under it all: who won and on what.
    xr_string footer;
    if (result.winner >= 0 && result.weight_sum > 0.0f)
    {
        string256 head;
        xr_sprintf(head, StringTable().translate("st_cmp_winner").c_str(), u32(result.winner + 1), items[result.winner]->NameShort());
        footer = head;
        if (!result.reasons.empty())
        {
            // the string table trims a text, so the separators are the code's
            footer += " - ";
            footer += StringTable().translate("st_cmp_reasons").c_str();
            footer += " ";
            for (size_t i = 0; i < result.reasons.size(); ++i)
            {
                if (i)
                    footer += ", ";
                footer += StringTable().translate(result.reasons[i]->caption).c_str();
            }
        }
    }
    else
        footer = StringTable().translate("st_cmp_no_weights").c_str();
    m_footer->SetText(footer.c_str());
    m_list->ScrollToBegin();
}

void CUIItemCompareWnd::Update()
{
    inherited::Update();
    // An item sold, dropped or used while the window is up leaves the comparison.
    for (const u16 id : m_ids)
    {
        if (!Level().Objects.net_Find(id))
        {
            m_ids.erase(std::remove(m_ids.begin(), m_ids.end(), id), m_ids.end());
            Rebuild();
            break;
        }
    }
}

void CUIItemCompareWnd::Draw()
{
    // the menu under it fades back
    if (m_shade.Visible())
        UIShape::Draw(Frect().set(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT), m_shade);
    inherited::Draw();
}

bool CUIItemCompareWnd::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    // a click beside the window closes it; nothing under it takes the mouse while it is up
    if (mouse_action == WINDOW_LBUTTON_DOWN || mouse_action == WINDOW_RBUTTON_DOWN)
    {
        Frect rect;
        GetAbsoluteRect(rect);
        Fvector2 cursor = GetUICursor().GetCursorPosition();
        if (!rect.in(cursor))
        {
            Close();
            return true;
        }
    }
    inherited::OnMouseAction(x, y, mouse_action);
    return true;
}

bool CUIItemCompareWnd::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
    if (keyboard_action == WINDOW_KEY_PRESSED)
    {
        if (IsBinded(kUI_TAB_PREV, dik, EKeyContext::UI) || IsBinded(kUI_TAB_NEXT, dik, EKeyContext::UI))
        {
            SetProfile(m_profile == Use ? Sale : Use);
            return true;
        }
    }
    return inherited::OnKeyboardAction(dik, keyboard_action);
}

void CUIItemCompareWnd::SendMessage(CUIWindow* pWnd, s16 msg, void* pData) { CUIWndCallback::OnEvent(pWnd, msg, pData); }

void CUIItemCompareWnd::OnTab(CUIWindow* w, void*)
{
    for (u8 i = 0; i < ProfileCount; ++i)
    {
        if (w == m_tabs[i])
            SetProfile(EProfile(i));
    }
}

void CUIItemCompareWnd::OnClose(CUIWindow*, void*) { Close(); }
