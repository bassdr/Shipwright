#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"

extern PlayState* gPlayState;
}

static constexpr int32_t CVAR_SKIP_WARP_DEFAULT = 0;
#define CVAR_SKIP_WARP_NAME CVAR_ENHANCEMENT("WarpSongSkipAnimation")
#define CVAR_SKIP_WARP_VALUE CVarGetInteger(CVAR_SKIP_WARP_NAME, CVAR_SKIP_WARP_DEFAULT)

// DEMOKANKYO_WARP_OUT = 0x0F; the warp-song departure animation spawned by Player_Action_8084E3C4
static constexpr s16 DEMOKANKYO_WARP_OUT_PARAM = 0x0F;

static void RegisterSkipWarpHooks() {
    COND_HOOK(OnActorInit, CVAR_SKIP_WARP_VALUE, [](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        if (actor->id == ACTOR_DEMO_KANKYO && actor->params == DEMOKANKYO_WARP_OUT_PARAM) {
            Environment_WarpSongLeave(gPlayState);
            // Switch arrival spawn mode from WARP_SONG to IDLE so DEMO_KANKYO WARP_IN is never spawned
            gSaveContext.respawn[RESPAWN_MODE_RETURN].playerParams =
                (gSaveContext.respawn[RESPAWN_MODE_RETURN].playerParams & ~(0xF << 8)) | (PLAYER_START_MODE_IDLE << 8);
            Actor_Kill(actor);
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterSkipWarpHooks, { CVAR_SKIP_WARP_NAME });
