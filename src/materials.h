#ifndef MATERIALS_H
#define MATERIALS_H

#include <stdbool.h>

#include <raylib.h>

#include "world.h"

/* Resting temperature of every cell. A cell more than half a degree away from
   it counts as thermally active and keeps its chunk awake, so fresh storage
   must start here — zeroed cells would read as hot and never let chunks sleep. */
#define AMBIENT_TEMPERATURE 20.0f

/* Everything a material *is* lives in this one table; only what a material
   *does* per tick stays as code. Adding a material used to mean finding seven
   separate switch statements, and a forgotten case failed silently. */
typedef struct MaterialPhase {
    bool enabled;
    CellMaterial target;
    float threshold;
} MaterialPhase;

typedef struct MaterialInfo {
    const char *name;
    Color color;
    /* Per-channel spread of the coordinate-hash variation, in halves, so a
       material can dither one channel harder than another. */
    signed char variationR;
    signed char variationG;
    signed char variationB;
    float initialTemperature;
    /* Relaxation toward selfHeatTarget, plus a flat per-tick drop for materials
       that simply cool off. */
    float selfHeatTarget;
    float selfHeatRate;
    float linearCoolRate;
    /* Thermal phase changes, one in each direction, because water both boils
       and freezes. `enabled` exists so that the zero value of a forgotten field
       is inert: encoding "no transition" as a target equal to the material
       itself looks tidy but means an unwritten field reads back as "become
       MATERIAL_EMPTY at 0C", and the cryo beam duly deleted every rock, dirt
       and sand cell it touched. */
    MaterialPhase onHeat;
    MaterialPhase onCool;
    bool dynamic;
    bool solid;
    /* How much light the material gives off by itself, 0..1. Heat adds more on
       top of this, so a laser-blasted rock face lights its own crater. */
    float emission;
    /* Degrees per second the laser pours into this material, and degrees per
       second the cryo beam pulls out of it. Zero means the beam does not work
       it. Keeping both here rather than in switches is what makes a new
       material one table entry: the first version of ice was solid, stopped
       nothing, and could not be melted, because the laser still asked for three
       material names by hand. */
    float laserHeatRate;
    float chillRate;
    /* Mass of one cell of this material, relative to water at 1.0. Real
       densities rounded to two figures, which is enough: nothing weighs a cell
       in kilograms, and every consumer only needs the ratios between materials
       to be believable — a slab of rock must fall harder and spin slower than
       the same slab of ice. Zero means the material has no mass, which is
       correct for empty space and for gases nothing can pick up. */
    float density;
} MaterialInfo;

extern const MaterialInfo MATERIALS[MATERIAL_COUNT];

/* Inline because the simulation asks for a material's properties several times
   per cell per tick; a call across a translation unit here is measurable. */
static inline const MaterialInfo *MaterialAt(CellMaterial material)
{
    if (material < 0 || material >= MATERIAL_COUNT) {
        return &MATERIALS[MATERIAL_EMPTY];
    }
    return &MATERIALS[material];
}

static inline bool MaterialIsDynamic(CellMaterial material)
{
    return MaterialAt(material)->dynamic;
}

static inline bool MaterialIsSolid(CellMaterial material)
{
    return MaterialAt(material)->solid;
}

static inline float MaterialInitialTemperature(CellMaterial material)
{
    return MaterialAt(material)->initialTemperature;
}

/* Fails on a table entry that cannot be simulated safely: a missing name, or a
   phase transition pointing at a material that does not exist. Cheap enough to
   run at startup and in tests, and it turns a whole class of "new material
   silently misbehaves" bugs into an immediate, named failure. */
bool MaterialsValidate(void);

#endif
