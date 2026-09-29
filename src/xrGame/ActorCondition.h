// ActorCondition.h: класс состояния игрока
//

#pragma once

#include "EntityCondition.h"
#include "actor_defs.h"

template <typename _return_type>
class CScriptCallbackEx;
class CActor;
class CActorDeathEffector;

class CActorCondition : public CEntityCondition
{
private:
    typedef CEntityCondition inherited;
    enum
    {
        eCriticalPowerReached = (1 << 0),
        eCriticalMaxPowerReached = (1 << 1),
        eCriticalBleedingSpeed = (1 << 2),
        eCriticalSatietyReached = (1 << 3),
        eCriticalRadiationReached = (1 << 4),
        eWeaponJammedReached = (1 << 5),
        ePhyHealthMinReached = (1 << 6),
        eCantWalkWeight = (1 << 7),
        eCantWalkWeightReached = (1 << 8),
    };
    Flags16 m_condition_flags;

private:
    CActor* m_object;
    CActorDeathEffector* m_death_effector;
    void UpdateTutorialThresholds();
    void UpdateSatiety();
    void UpdateRadiation() override;
    float GetActiveBoosterValue(EBoostParams type) const;

    // One short stretch of what time does to the body; worn: the belt, the helmet and the suit
    // act inside it too (a frame leaves them to the actor's own accumulator).
    void IntegrateStep(float dt, bool worn);
    void IntegrateTimeJump(float game_seconds);
    // Environmental hits (fields, zones) per real second, decayed over a couple of seconds: what
    // would keep coming for as long as the actor lay there.
    struct SExposure
    {
        float radiation{};
        float psy{};
        float health{};
        float at{};
    };
    SExposure m_exposure;
    void DecayExposure();

public:
    CActorCondition(CActor* object);
    virtual ~CActorCondition();

    virtual void LoadCondition(LPCSTR section);
    virtual void reinit();

    virtual CWound* ConditionHit(SHit* pHDS);
    // The health a hit of this power would take, through the outfit and the immunities, without
    // dealing it. The landing roll asks before it commits: a roll into a lethal drop is no roll.
    float PredictHealthLoss(float hit_power, ALife::EHitType hit_type, s16 element);
    virtual void UpdateCondition();
    // indicators: false inside a long integration, which refreshes the HUD once at its end.
    void UpdateBoosters(bool indicators = true);

    // What a long stretch of game time would do to the actor lying still - a sleep. Runs the very
    // integration the real jump runs (IntegrateStep), on the live state, and puts the state back.
    enum ESleepThreat : u8
    {
        eSleepThreatNone = 0,
        eSleepThreatRadiation,
        eSleepThreatBleeding,
        eSleepThreatHunger,
        eSleepThreatZone,
    };
    struct SSleepForecast
    {
        float min_health{1.f}; // the lowest health on the way; 0 or below is death
        float death_at{-1.f}; // game seconds into the stretch, negative when it is survived
        ESleepThreat cause{eSleepThreatNone}; // what took the most health
    };
    // satiety_add, psy_add, radiation_add: what the script layer itself hands the actor at
    // bedtime, which the first step of the real jump takes as well.
    SSleepForecast ForecastTimeJump(float game_seconds, float satiety_add, float psy_add, float radiation_add);
    const SSleepForecast& LastForecast() const { return m_last_forecast; }

    virtual void ChangeAlcohol(const float value);
    virtual void ChangeSatiety(const float value);

    void BoostParameters(const SBooster& B);
    void DisableBoostParameters(const SBooster& B);
    void WoundForEach(const luabind::functor<bool>& funct);
    void BoosterForEach(const luabind::functor<bool>& funct);
    bool ApplyBooster_script(const SBooster& B, LPCSTR sect);
    void ClearAllBoosters();
    void BoostMaxWeight(const float value);
    void BoostHpRestore(const float value);
    void BoostPowerRestore(const float value);
    void BoostRadiationRestore(const float value);
    void BoostBleedingRestore(const float value);
    void BoostBurnImmunity(const float value);
    void BoostShockImmunity(const float value);
    void BoostRadiationImmunity(const float value);
    void BoostTelepaticImmunity(const float value);
    void BoostChemicalBurnImmunity(const float value);
    void BoostExplImmunity(const float value);
    void BoostStrikeImmunity(const float value);
    void BoostFireWoundImmunity(const float value);
    void BoostWoundImmunity(const float value);
    void BoostRadiationProtection(const float value);
    void BoostTelepaticProtection(const float value);
    void BoostChemicalBurnProtection(const float value);
    const auto& GetCurBoosterInfluences() const { return m_booster_influences; }
    // хромание при потере сил и здоровья
    virtual bool IsLimping() const;
    virtual bool IsCantWalk() const;
    virtual bool IsCantWalkWeight();
    virtual bool IsCantSprint() const;

    void PowerHit(float power, bool apply_outfit);
    float GetPower() const { return m_fPower; }
    void ConditionJump(float weight);
    void ConditionWalk(float weight, bool accel, bool sprint);
    void ConditionStand(float weight);
    IC float MaxWalkWeight() const { return m_MaxWalkWeight; }
    float GetAlcohol() { return m_fAlcohol; }
    float GetPsy() { return 1.0f - GetPsyHealth(); }
    float GetSatiety() const { return m_fSatiety; }
    IC float GetSatietyPower() const { return m_fV_SatietyPower * m_fSatiety; };
    void AffectDamage_InjuriousMaterialAndMonstersInfluence();
    float GetInjuriousMaterialDamage();

    void SetZoneDanger(float danger, ALife::EInfluenceType type);
    float GetZoneDanger() const;

public:
    IC CActor& object() const
    {
        VERIFY(m_object);
        return (*m_object);
    }
    virtual void save(NET_Packet& output_packet);
    virtual void load(IReader& input_packet);
    //	IC		float const&	Satiety					()	{ return m_fSatiety; }
    IC float const& V_Satiety() { return m_fV_Satiety; }
    IC float const& V_SatietyPower() { return m_fV_SatietyPower; }
    IC float const& V_SatietyHealth() { return m_fV_SatietyHealth; }
    IC float const& SatietyCritical() { return m_fSatietyCritical; }
    float GetZoneMaxPower(ALife::EInfluenceType type) const;
    float GetZoneMaxPower(ALife::EHitType hit_type) const;
    float GetItemEffectTimeScale() const;

    bool DisableSprint(SHit* pHDS);
    bool PlayHitSound(SHit* pHDS);
    float HitSlowmo(SHit* pHDS);
    virtual bool ApplyInfluence(const SMedicineInfluenceValues& V, const shared_str& sect);
    virtual bool ApplyBooster(const SBooster& B, const shared_str& sect);
    float GetMaxPowerRestoreSpeed() const { return m_max_power_restore_speed; };
    float GetMaxWoundProtection() const { return m_max_wound_protection; };
    float GetMaxFireWoundProtection() const { return m_max_fire_wound_protection; };
protected:
    SSleepForecast m_last_forecast;
    SMedicineInfluenceValues m_curr_medicine_influence;
    float m_fAlcohol;
    float m_fV_Alcohol;
    //--
    float m_fSatiety;
    float m_fV_Satiety;
    float m_fV_SatietyPower;
    float m_fV_SatietyHealth;
    float m_fSatietyCritical;
    //--
    float m_fPowerLeakSpeed;

    float m_fJumpPower;
    float m_fStandPower;
    float m_fWalkPower;
    float m_fJumpWeightPower;
    float m_fWalkWeightPower;
    float m_fOverweightWalkK;
    float m_fOverweightJumpK;
    float m_fAccelK;
    float m_fSprintK;

public:
    float m_MaxWalkWeight;

protected:
    float m_zone_max_power[ALife::infl_max_count];
    float m_zone_danger[ALife::infl_max_count];
    float m_f_time_affected;
    float m_max_power_restore_speed;
    float m_max_wound_protection;
    float m_max_fire_wound_protection;

    mutable bool m_bLimping{ false };
    mutable bool m_bCantWalk{ false };
    mutable bool m_bCantSprint{ false };

    //порог силы и здоровья меньше которого актер начинает хромать
    float m_fLimpingPowerBegin;
    float m_fLimpingPowerEnd;
    float m_fCantWalkPowerBegin;
    float m_fCantWalkPowerEnd;

    float m_fCantSprintPowerBegin;
    float m_fCantSprintPowerEnd;

    float m_fLimpingHealthBegin;
    float m_fLimpingHealthEnd;

    // typedef xr_vector<SMedicineInfluenceValues> BOOSTS_VECTOR;
    // typedef xr_vector<SMedicineInfluenceValues>::iterator BOOSTS_VECTOR_ITER;
    // BOOSTS_VECTOR m_vecBoosts;
    ref_sound m_use_sound;

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

class CActorDeathEffector
{
    CActorCondition* m_pParent;
    ref_sound m_death_sound;
    bool m_b_actual;
    float m_start_health;
    void OnPPEffectorReleased();

public:
    CActorDeathEffector(CActorCondition* parent, LPCSTR sect); // -((
    ~CActorDeathEffector();
    void UpdateCL();
    IC bool IsActual() { return m_b_actual; }
    void Stop();
};
