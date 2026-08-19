#pragma once

namespace GameObject
{
enum ECallbackType : u32
{
    eTradeStart = u32(0),
    eTradeStop,
    eTradeSellBuyItem,
    eTradePerformTradeOperation,

    eZoneEnter,
    eZoneExit,
    eExitLevelBorder,
    eEnterLevelBorder,
    eDeath,

    ePatrolPathInPoint,

    eInventoryPda,
    eInventoryInfo,
    eArticleInfo,
    eTaskStateChange,
    eMapLocationAdded,

    eUseObject,

    eHit,

    eSound,

    eActionTypeMovement,
    eActionTypeWatch,
    eActionTypeRemoved,
    eActionTypeAnimation,
    eActionTypeSound,
    eActionTypeParticle,
    eActionTypeObject,

    eActorSleep,

    eHelicopterOnPoint,
    eHelicopterOnHit,

    eOnItemTake,
    eOnItemDrop,

    eScriptAnimation,

    eTraderGlobalAnimationRequest,
    eTraderHeadAnimationRequest,
    eTraderSoundEnd,

    eInvBoxItemTake,
    eWeaponNoAmmoAvailable,

    //Alundaio: added defines
    eActorHudAnimationEnd,

    eTakeItemFromGround,

    //AVO: custom callbacks
    // Input
    eKeyPress,
    eKeyRelease,
    eKeyHold,
    // Inventory
    eItemToBelt,
    eItemToSlot,
    eItemToRuck,
    // weapon
    eOnWeaponZoomIn,
    eOnWeaponZoomOut,
    eOnWeaponJammed,
    eOnWeaponMagazineEmpty,
    // Actor
    eActorBeforeDeath,
    // vehicle
    eAttachVehicle,
    eDetachVehicle,
    eUseVehicle,
    //-AVO

    eControllerPress,
    eControllerRelease,
    eControllerHold,
    eControllerAttitudeChange,

    // A step of a task changing state. Its own id, because the task callback's contract is
    // (task, state) and every script written for it reads the second argument as the state -
    // handing an objective over on that same id made a finished task announce itself as a new
    // one. Appended with an explicit number so no existing id shifts.
    eTaskObjectiveStateChange = 125,

    // X-Ray Extensions:
    eMouseWheel = 126,
    eMouseMove  = 127,
    eActorLand1 = 128,
    eActorLand2 = 129,

    eDummy = u32(-1),
};
};
