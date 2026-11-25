#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

/**
 * ACTOR_STATE
 *
 * Contains real-time state for actors that keep their state outside of flags
 * (push blocks, runtime-spawned objects like item drops, and enemy despawns),
 * which would otherwise diverge between clients in the same scene
 *
 * Broadcast to other clients within the same scene when a synced actor moves,
 * spawns, or despawns, and periodically re-broadcast while it deviates from
 * its spawn position
 *
 * Note: This packet can be sent _a lot_ while an actor is moving, so please do
 * not include any unnecessary data in it
 */

static const AnchorSyncedActorConfig sSyncedActors[] = {
    // Push/pull blocks. Position is replicated since it is only stored in runtime fields
    { ACTOR_OBJ_OSHIHIKI, true, true },
    // Item drops (rupees, hearts, bombs etc.). Spawn/despawn is replicated so drops from
    // remotely-triggered events appear; positions are left to the local simulation
    { ACTOR_EN_ITEM00, false, true },
};

const AnchorSyncedActorConfig* Anchor::GetSyncedActorConfig(s16 actorId) {
    for (size_t i = 0; i < ARRAY_COUNT(sSyncedActors); i++) {
        if (sSyncedActors[i].actorId == actorId) {
            return &sSyncedActors[i];
        }
    }

    return nullptr;
}

static bool HasPlayerInScene() {
    for (auto& [clientId, client] : Anchor::Instance->clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            return true;
        }
    }

    return false;
}

static void SendToClientsInScene(nlohmann::json& payload) {
    for (auto& [clientId, client] : Anchor::Instance->clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            Anchor::Instance->SendJsonToRemote(payload);
        }
    }
}

void Anchor::SendPacket_ActorState(Actor* actor, Vec3f spawnPos) {
    if (!IsSaveLoaded() || !HasPlayerInScene()) {
        return;
    }

    nlohmann::json payload;

    payload["type"] = ACTOR_STATE;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["roomNum"] = (s32)actor->room;
    payload["actorId"] = actor->id;
    payload["params"] = actor->params;
    payload["spawnPos"] = spawnPos;
    payload["pos"] = actor->world.pos;
    payload["rot"] = actor->world.rot;
    payload["quiet"] = true;

    SendToClientsInScene(payload);
}

void Anchor::SendPacket_ActorDespawn(Actor* actor, Vec3f spawnPos) {
    if (!IsSaveLoaded() || !HasPlayerInScene()) {
        return;
    }

    nlohmann::json payload;

    payload["type"] = ACTOR_STATE;
    payload["despawn"] = true;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["roomNum"] = (s32)actor->room;
    payload["actorId"] = actor->id;
    payload["params"] = actor->params;
    payload["spawnPos"] = spawnPos;
    payload["quiet"] = true;

    SendToClientsInScene(payload);
}

static Actor* FindSyncedActor(s16 actorId, s16 params, Vec3f spawnPos) {
    for (s32 i = 0; i < ACTORCAT_MAX; i++) {
        Actor* actor = gPlayState->actorCtx.actorLists[i].head;

        while (actor != nullptr) {
            if (actor->id == actorId && actor->params == params) {
                AnchorActorSyncData* sync =
                    ObjectExtension::GetInstance().Get<AnchorActorSyncData>(actor);
                Vec3f key = sync != nullptr ? sync->spawnPos : actor->home.pos;

                if (Math_Vec3f_DistXYZ(&key, &spawnPos) < 1.0f) {
                    return actor;
                }
            }
            actor = actor->next;
        }
    }

    return nullptr;
}

void Anchor::HandlePacket_ActorState(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    if (payload.value("sceneNum", (s16)SCENE_ID_MAX) != gPlayState->sceneNum) {
        return;
    }

    s16 actorId = payload.value("actorId", (s16)0);
    s16 params = payload.value("params", (s16)0);
    s8 roomNum = payload.value("roomNum", (s8)-1);
    Vec3f spawnPos = payload.value("spawnPos", Vec3f{ 0 });

    if (payload.value("despawn", false)) {
        Actor* actor = FindSyncedActor(actorId, params, spawnPos);

        if (actor != nullptr) {
            // Mark so our own OnActorKill hook doesn't echo the despawn back. A frame
            // stamp can't be used here since the position heartbeat keeps it fresh,
            // which would also suppress legitimate local kills
            isApplyingRemoteDespawn = true;
            Actor_Kill(actor);
            isApplyingRemoteDespawn = false;
        }
        return;
    }

    Vec3f pos = payload.value("pos", Vec3f{ 0 });
    Vec3s rot = payload.value("rot", Vec3s{ 0 });

    Actor* actor = FindSyncedActor(actorId, params, spawnPos);

    // The actor may not exist locally if it was spawned at runtime on the remote client
    // (e.g. item drops, blocks spawned by Obj_Makeoshihiki). Spawn it if its room is
    // currently loaded, positioned at its original spawn position so the
    // AnchorActorSyncData identity stays consistent
    if (actor == nullptr) {
        const AnchorSyncedActorConfig* config = GetSyncedActorConfig(actorId);
        if (config != nullptr && config->broadcastSpawn && roomNum == gPlayState->roomCtx.curRoom.num) {
            isSpawningRemoteActor = true;
            actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId, spawnPos.x, spawnPos.y, spawnPos.z,
                                rot.x, rot.y, rot.z, params);
            isSpawningRemoteActor = false;
        }
    }

    if (actor == nullptr) {
        return;
    }

    // For enemies only the room broadcaster's simulation is applied. If that's us,
    // ignore remote positions so their stream doesn't fight our simulation
    if (actor->category == ACTORCAT_ENEMY && IsActorSyncBroadcaster(actor->room)) {
        return;
    }

    actor->world.pos = pos;
    actor->prevPos = pos;
    actor->home.pos = pos;
    actor->world.rot = rot;
    actor->shape.rot = rot;
    actor->home.rot = rot;

    AnchorActorSyncData* sync = ObjectExtension::GetInstance().Get<AnchorActorSyncData>(actor);
    if (sync != nullptr) {
        sync->lastBroadcastPos = pos;
        sync->lastBroadcastRot = rot;
        sync->remoteUpdateFrame = gPlayState->gameplayFrames;
    }
}
