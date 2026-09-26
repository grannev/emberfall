#ifndef FAUNA_H
#define FAUNA_H

/* Fauna — groundwork only. Nothing lives in the world yet.
 *
 * The generator marks habitats (`World.habitats`): a nest in a crown, a
 * meadow, a burrow in the dunes, a den on the frost, a vent in the ember
 * wastes, a reef on the sea floor, a roost under a cave ceiling. This module
 * names what would come from each and keeps track of which are near the
 * character, so the HUD and the sound stubs can already say so. When animals
 * arrive they will be spawned from these marks, updated here on the fixed
 * step and drawn by a renderer module of their own; until then there is no
 * creature, no update of one and nothing drawn.
 */

#include <raylib.h>

#include "world.h"

typedef enum FaunaKind {
    FAUNA_SONGBIRD = 0,
    FAUNA_HARE,
    FAUNA_BEETLE,
    FAUNA_SNOW_FOX,
    FAUNA_SALAMANDER,
    FAUNA_CRAB,
    FAUNA_BAT,
    FAUNA_KIND_COUNT
} FaunaKind;

/* How far from the character a habitat counts as near, in cells. */
#define FAUNA_NEAR_REACH 480.0f

typedef struct FaunaSystem {
    /* Habitats within reach of the character at the last update, by kind of
       animal, and the nearest one's animal (or -1). */
    int nearby[FAUNA_KIND_COUNT];
    int nearest;
} FaunaSystem;

void FaunaInit(FaunaSystem *fauna);
/* What would live in a habitat of this kind. */
FaunaKind FaunaKindForHabitat(WorldHabitatKind habitat);
const char *FaunaKindName(FaunaKind kind);
/* Counts the habitats near `around`. A scan of the habitat list, bounded by
   WORLD_MAX_HABITATS; it never reads a cell. */
void FaunaUpdate(FaunaSystem *fauna, const World *world, Vector2 around);

#endif
