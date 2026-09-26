#ifndef MATERIALS_H
#define MATERIALS_H

#include <stdbool.h>

#include <raylib.h>

#include "world.h"

/* Resting temperature of every material. A cell more than half a degree away
   from its material's rest counts as thermally active and keeps its chunk
   awake. Empty cells have no temperature at all — the field is ignored for
   them and reads back as ambient — which is what lets fresh storage stay
   untouched: a zeroed cell is an empty cell at rest. */
#define AMBIENT_TEMPERATURE 20.0f

/* Everything a material *is* lives in this one table; only what a material
   *does* per tick stays as code. Adding a material used to mean finding seven
   separate switch statements, and a forgotten case failed silently. */
typedef struct MaterialPhase {
    bool enabled;
    CellMaterial target;
    float threshold;
} MaterialPhase;

/* How a material's tones are laid out over its cells. Generic patterns, not
   a switch per material: a new material picks one and gives its colours. */
typedef enum MaterialPattern {
    /* Every cell its own tone, carried by the cell as it moves: sand, ash,
       rubble — a pile of grains, each a slightly different stone. */
    MATERIAL_PATTERN_GRAIN = 0,
    /* Tones in clumps a few cells across, with grain on top: soil, a canopy,
       a turf. */
    MATERIAL_PATTERN_CLUMP,
    /* Bands that run roughly along the rows and wander: bedded rock. */
    MATERIAL_PATTERN_STRATA,
    /* Streaks along the columns: the grain of a trunk, the ribs of a cactus,
       blades of grass. */
    MATERIAL_PATTERN_FIBRE,
    /* Long diagonal glints: ice. */
    MATERIAL_PATTERN_CRYSTAL,
    /* Soft broad swirls and a lit surface, darkening with depth: water,
       lava. The depth and the surface come from the neighbours. */
    MATERIAL_PATTERN_FLUID,
    /* Laid courses: bricks five cells long and two high, each its own tone,
       in a running bond with dark mortar between. */
    MATERIAL_PATTERN_BRICK,
    /* Riveted plates ten cells by seven, a dark seam between them and a
       rivet at each corner, each plate its own tone with a brushed streak
       along it: hull and panel. */
    MATERIAL_PATTERN_PLATE,
    /* Dressed stone in great blocks eight by five in a running bond, thin
       joints, and every fourth course a band with a stepped key carved
       along it: precursor masonry. */
    MATERIAL_PATTERN_ASHLAR,
    /* Blades: the tone is the cell's shade read straight, dark to light, so
       whatever lays a blade gives it a dark foot and a bright tip; the
       lowest shades are the accent — a flower on the tip. */
    MATERIAL_PATTERN_BLADE,
} MaterialPattern;

typedef struct MaterialInfo {
    const char *name;
    Color color;
    /* The rest of the palette around `color`: a darker and a lighter tone the
       pattern moves between, and an accent a small share of cells take — a
       pebble in the soil, a vein in the rock, a knot in the wood, a spark in
       the lava. `accentShare` is out of 64. A material with no palette
       given (all zero) falls back to `color` and its variation. */
    Color dark;
    Color light;
    Color accent;
    unsigned char accentShare;
    MaterialPattern pattern;
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
    /* Flows and seeks a level, as opposed to piling up or rising. What the
       motion rules ask when they need to know whether a cell is pressing on
       the one below it. */
    bool liquid;
    /* Grows on the ground rather than being it. Solid — a canopy can be stood
       on and a trunk can be cut — but never the answer to "where is the
       surface here": a tree is not a cliff, and code that measures terrain has
       to be able to say so. */
    bool flora;
    /* Stands behind the character, as the plants do: a girder under an
       outpost, a column in a hall. The character and the bodies pass
       through it, the light passes through it, and it is solid to
       everything else — the drill, the fire, a blast, what holds a roof up.
       Every plant is backdrop too, without saying so. */
    bool backdrop;
    /* Lives in the decor plane (`World.decor`), never in a cell: a column, a
       girder, a plank, a lamp, a strand of kelp. The simulation does not see
       it at all — water fills the cell in front of it, the character and
       the bodies pass it — but it is drawn, it gives light, and a roof
       resting on it is held up by it. */
    bool decor;
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
    /* How many cells of ceiling this material can hold up between two
       supports before it crumbles, checked only where something was just
       destroyed (terrain_stability.h). Zero means it never crumbles: a
       liquid, a gas, and anything that falls on its own anyway. Rock spans a
       cavern; dirt spans a burrow; a leaf canopy holds itself up on nothing,
       which is what makes it a canopy. */
    int span;
} MaterialInfo;

extern const MaterialInfo MATERIALS[MATERIAL_COUNT];

static inline bool MaterialIsFlora(CellMaterial material)
{
    if (material < 0 || material >= MATERIAL_COUNT) return false;
    return MATERIALS[material].flora;
}

/* Whether the material stands behind the character: every plant, and what
   the builders put up to be walked past rather than into. */
static inline bool MaterialIsDecor(CellMaterial material)
{
    if (material < 0 || material >= MATERIAL_COUNT) return false;
    return MATERIALS[material].decor;
}

static inline bool MaterialIsBackdrop(CellMaterial material)
{
    if (material < 0 || material >= MATERIAL_COUNT) return false;
    return MATERIALS[material].flora || MATERIALS[material].backdrop;
}

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

static inline bool MaterialIsLiquid(CellMaterial material)
{
    return MaterialAt(material)->liquid;
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
