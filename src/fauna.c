/* Fauna groundwork. See fauna.h. */
#include "fauna.h"

#include <math.h>
#include <stddef.h>

void FaunaInit(FaunaSystem *fauna)
{
    int kind;

    if (fauna == NULL) return;
    for (kind = 0; kind < FAUNA_KIND_COUNT; ++kind) fauna->nearby[kind] = 0;
    fauna->nearest = -1;
}

FaunaKind FaunaKindForHabitat(WorldHabitatKind habitat)
{
    static const FaunaKind animals[WORLD_HABITAT_KIND_COUNT] = {
        [WORLD_HABITAT_NEST] = FAUNA_SONGBIRD,  [WORLD_HABITAT_MEADOW] = FAUNA_HARE,
        [WORLD_HABITAT_BURROW] = FAUNA_BEETLE,  [WORLD_HABITAT_DEN] = FAUNA_SNOW_FOX,
        [WORLD_HABITAT_VENT] = FAUNA_SALAMANDER, [WORLD_HABITAT_REEF] = FAUNA_CRAB,
        [WORLD_HABITAT_ROOST] = FAUNA_BAT,
    };

    return habitat >= 0 && habitat < WORLD_HABITAT_KIND_COUNT ? animals[habitat]
                                                              : FAUNA_SONGBIRD;
}

const char *FaunaKindName(FaunaKind kind)
{
    static const char *const names[FAUNA_KIND_COUNT] = {
        [FAUNA_SONGBIRD] = "SONGBIRD", [FAUNA_HARE] = "HARE",
        [FAUNA_BEETLE] = "BEETLE",     [FAUNA_SNOW_FOX] = "SNOW FOX",
        [FAUNA_SALAMANDER] = "SALAMANDER", [FAUNA_CRAB] = "CRAB",
        [FAUNA_BAT] = "BAT",
    };

    return kind >= 0 && kind < FAUNA_KIND_COUNT ? names[kind] : "UNKNOWN";
}

void FaunaUpdate(FaunaSystem *fauna, const World *world, Vector2 around)
{
    float nearestDistance = FAUNA_NEAR_REACH * FAUNA_NEAR_REACH;
    int index;

    if (fauna == NULL || world == NULL) return;
    FaunaInit(fauna);
    for (index = 0; index < world->habitatCount; ++index) {
        const WorldHabitat *habitat = &world->habitats[index];
        float width = (float)world->width;
        float dx = fmodf((float)habitat->x - around.x, width);
        float dy = (float)habitat->y - around.y;
        float distance;
        FaunaKind kind;

        if (dx > width * 0.5f) dx -= width;
        if (dx < -width * 0.5f) dx += width;
        distance = dx * dx + dy * dy;
        if (distance > FAUNA_NEAR_REACH * FAUNA_NEAR_REACH) continue;
        kind = FaunaKindForHabitat((WorldHabitatKind)habitat->kind);
        ++fauna->nearby[kind];
        if (distance <= nearestDistance) {
            nearestDistance = distance;
            fauna->nearest = (int)kind;
        }
    }
}
