/* The material table: the single source of truth for what every material is.
 *
 * Adding a material means adding one entry here plus, only if it needs
 * behaviour no existing material has, a case in world_simulation.c. Anything
 * expressed as a number — colour, density of dither, thermal thresholds, what a
 * beam does to it — belongs in this table, because a property that lives in a
 * switch statement somewhere else is a property the next material will forget.
 */
#include "materials.h"

#include <stddef.h>

const MaterialInfo MATERIALS[MATERIAL_COUNT] = {
    [MATERIAL_EMPTY] = {
        .name = "EMPTY", .color = {5, 10, 18, 255},
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
            },
    [MATERIAL_DIRT] = {
        .name = "DIRT", .color = {111, 73, 43, 255},
        .dark = {78, 50, 30, 255}, .light = {142, 99, 62, 255},
        .accent = {150, 140, 124, 255}, .accentShare = 4,
        .pattern = MATERIAL_PATTERN_CLUMP,
        .variationR = 2, .variationG = 1,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .onHeat = {true, MATERIAL_FIRE, 175.0f},
        .solid = true,
        .laserHeatRate = 2500.0f,
        .chillRate = 260.0f,
        .density = 1.4f,
        .span = 8,
    },
    [MATERIAL_ROCK] = {
        .name = "ROCK", .color = {72, 77, 86, 255},
        .dark = {50, 54, 62, 255}, .light = {101, 106, 116, 255},
        .accent = {128, 118, 104, 255}, .accentShare = 3,
        .pattern = MATERIAL_PATTERN_STRATA,
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .onHeat = {true, MATERIAL_LAVA, 720.0f},
        .solid = true,
        .laserHeatRate = 1080.0f,
        .chillRate = 260.0f,
        .density = 2.6f,
        .span = 28,
    },
    [MATERIAL_SAND] = {
        .name = "SAND", .color = {218, 184, 91, 255},
        .dark = {186, 148, 68, 255}, .light = {240, 214, 136, 255},
        .accent = {150, 112, 66, 255}, .accentShare = 5,
        .pattern = MATERIAL_PATTERN_GRAIN,
        .variationR = 2, .variationG = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .onHeat = {true, MATERIAL_EMPTY, 280.0f},
        .dynamic = true, .solid = true,
        .laserHeatRate = 3100.0f,
        .chillRate = 260.0f,
        .density = 1.6f,
    },
    [MATERIAL_WATER] = {
        .name = "WATER", .color = {32, 111, 190, 225},
        .dark = {16, 62, 132, 235}, .light = {92, 170, 228, 215},
        .accent = {170, 214, 244, 215}, .accentShare = 2,
        .pattern = MATERIAL_PATTERN_FLUID,
        .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .onHeat = {true, MATERIAL_STEAM, 108.0f},
        .onCool = {true, MATERIAL_ICE, -4.0f},
        .dynamic = true, .liquid = true,
        .chillRate = 90.0f,
        .density = 1.0f,
    },
    [MATERIAL_LAVA] = {
        .name = "LAVA", .color = {245, 73, 18, 255},
        .dark = {178, 32, 10, 255}, .light = {255, 150, 40, 255},
        .accent = {255, 226, 120, 255}, .accentShare = 5,
        .pattern = MATERIAL_PATTERN_FLUID,
        .variationG = 4,
        .initialTemperature = 900.0f,
        .selfHeatTarget = 900.0f, .selfHeatRate = 0.08f,
                /* Lava will not cool this far on its own — it relaxes back toward 900 —
           so this threshold only ever fires under the cryo beam. */
        .onCool = {true, MATERIAL_ROCK, 620.0f},
        .dynamic = true, .liquid = true,
        .emission = 1.0f,
        /* Lava is pulled back toward 900C at 8% of the gap every tick, which at
           the 620C freezing point is 22 degrees a tick on its own. A beam that
           does not clearly beat that number does not cool lava at all — it just
           finds an equilibrium above the threshold and sits there. */
        .chillRate = 1900.0f,
        .density = 2.9f,
    },
    [MATERIAL_STEAM] = {
        .name = "STEAM", .color = {204, 222, 229, 178},
        .dark = {170, 188, 198, 160}, .light = {232, 242, 246, 190},
        .accent = {250, 252, 255, 200}, .accentShare = 3,
        .pattern = MATERIAL_PATTERN_GRAIN,
        .variationR = 2, .variationG = 2,
        .initialTemperature = 125.0f,
        .linearCoolRate = 0.42f,
        /* Steam thins into nothing when its life runs out rather than
           condensing back: a cloud of it raining down as water read as the
           water coming back, and the player asked for it to disperse. */
        .dynamic = true,
        .chillRate = 320.0f,
    },
    [MATERIAL_SMOKE] = {
        .name = "SMOKE", .color = {83, 88, 94, 205},
        .dark = {58, 62, 68, 205}, .light = {112, 116, 122, 200},
        .accent = {130, 124, 118, 200}, .accentShare = 2,
        .pattern = MATERIAL_PATTERN_GRAIN,
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = 75.0f,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .dynamic = true,
        .chillRate = 260.0f,
    },
    [MATERIAL_FIRE] = {
        .name = "FIRE", .color = {255, 132, 24, 245},
        .dark = {226, 70, 14, 245}, .light = {255, 196, 64, 245},
        .accent = {255, 244, 170, 250}, .accentShare = 8,
        .pattern = MATERIAL_PATTERN_GRAIN,
        .variationG = 6,
        .initialTemperature = 650.0f,
        .selfHeatTarget = 650.0f, .selfHeatRate = 0.12f,
        /* Chilled fire is put out and leaves smoke, the same residue it leaves
           when it burns out on its own. Fire relaxes back toward 650C, so
           nothing but the cryo beam ever reaches this. */
        .onCool = {true, MATERIAL_SMOKE, 120.0f},
        .dynamic = true,
        .emission = 0.92f,
        .chillRate = 2200.0f,
    },
    [MATERIAL_ICE] = {
        .name = "ICE", .color = {152, 203, 231, 245},
        .dark = {112, 168, 208, 245}, .light = {200, 234, 250, 245},
        .accent = {244, 252, 255, 250}, .accentShare = 4,
        .pattern = MATERIAL_PATTERN_CRYSTAL,
        .variationG = 2, .variationB = 2,
        .initialTemperature = -14.0f,
        /* Ice does not drift back to ambient. A slow drift cannot work here: a
           cell whose temperature moves less than the sleep threshold each tick
           never wakes its own chunk, so it would simply stop being simulated and
           the ice would be permanent anyway, only unpredictably so. Making it
           stable on purpose is honest and gives the player the one thing no
           other power does — a way to add material to the world. Anything warm
           still melts it: a laser, a fire, a lava flow. */
        .selfHeatTarget = -14.0f, .selfHeatRate = 0.0f,
        .onHeat = {true, MATERIAL_WATER, 2.0f},
        .solid = true,
        .laserHeatRate = 600.0f,
        .chillRate = 260.0f,
        .density = 0.92f,
        .span = 14,
    },
    [MATERIAL_WOOD] = {
        .flora = true,
        .name = "WOOD", .color = {104, 72, 44, 255},
        .dark = {72, 48, 28, 255}, .light = {136, 98, 62, 255},
        .accent = {56, 36, 22, 255}, .accentShare = 2,
        .pattern = MATERIAL_PATTERN_FIBRE,
        .variationR = 6, .variationG = 4, .variationB = 3,
        .initialTemperature = 18.0f,
        .selfHeatTarget = 18.0f, .selfHeatRate = 0.02f,
        /* Burns rather than melts, and at a temperature a laser reaches quickly
           and a lava flow reaches on contact. A forest beside a volcanic seam
           is supposed to be a hazard. */
        .onHeat = {true, MATERIAL_FIRE, 240.0f},
        .solid = true,
        .laserHeatRate = 900.0f,
        .chillRate = 120.0f,
        /* Light enough that a torn-off branch tumbles rather than drops, and
           far lighter than the rock it grows on. */
        .density = 0.55f,
        /* A branch, not a beam: long enough that a tree stands, short enough
           that a felled trunk bridging a chasm breaks under its own length. */
        .span = 18,
    },
    [MATERIAL_LEAF] = {
        .flora = true,
        .name = "LEAF", .color = {74, 132, 66, 245},
        .dark = {44, 94, 46, 245}, .light = {112, 170, 82, 245},
        .accent = {168, 196, 92, 245}, .accentShare = 4,
        .pattern = MATERIAL_PATTERN_CLUMP,
        .variationR = 5, .variationG = 9, .variationB = 4,
        .initialTemperature = 16.0f,
        .selfHeatTarget = 16.0f, .selfHeatRate = 0.02f,
        /* Foliage goes first: it catches at well under what the trunk needs. */
        .onHeat = {true, MATERIAL_FIRE, 150.0f},
        .solid = true,
        .laserHeatRate = 1100.0f,
        .chillRate = 150.0f,
        .density = 0.22f,
    },
    [MATERIAL_GRASS] = {
        .flora = true,
        .name = "GRASS", .color = {96, 148, 68, 240},
        .dark = {62, 112, 50, 240}, .light = {140, 188, 88, 240},
        .accent = {214, 196, 92, 240}, .accentShare = 3,
        .pattern = MATERIAL_PATTERN_FIBRE,
        .variationR = 6, .variationG = 10, .variationB = 5,
        .initialTemperature = 16.0f,
        .selfHeatTarget = 16.0f, .selfHeatRate = 0.02f,
        .onHeat = {true, MATERIAL_FIRE, 130.0f},
        .solid = true,
        .laserHeatRate = 1200.0f,
        .chillRate = 160.0f,
        .density = 0.18f,
    },
    [MATERIAL_CACTUS] = {
        .flora = true,
        .name = "CACTUS", .color = {68, 124, 82, 250},
        .dark = {46, 94, 62, 250}, .light = {100, 156, 108, 250},
        .accent = {226, 224, 186, 250}, .accentShare = 3,
        .pattern = MATERIAL_PATTERN_FIBRE,
        .variationR = 4, .variationG = 8, .variationB = 5,
        .initialTemperature = 26.0f,
        .selfHeatTarget = 26.0f, .selfHeatRate = 0.02f,
        /* Full of water, so it takes far more heat than wood before it gives
           up, which is the whole point of the thing in a desert. */
        .onHeat = {true, MATERIAL_STEAM, 420.0f},
        .solid = true,
        .laserHeatRate = 700.0f,
        .chillRate = 200.0f,
        .density = 0.68f,
    },
    [MATERIAL_ASH] = {
        .name = "ASH", .color = {112, 108, 104, 255},
        .dark = {84, 80, 78, 255}, .light = {142, 138, 132, 255},
        .accent = {176, 96, 58, 255}, .accentShare = 2,
        .pattern = MATERIAL_PATTERN_GRAIN,
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .dynamic = true,
        .chillRate = 260.0f,
        .density = 0.7f,
    },
    [MATERIAL_RUBBLE] = {
        .name = "RUBBLE", .color = {92, 82, 72, 255},
        .dark = {64, 57, 50, 255}, .light = {122, 111, 98, 255},
        .accent = {120, 124, 132, 255}, .accentShare = 10,
        .pattern = MATERIAL_PATTERN_GRAIN,
        .variationR = 4, .variationG = 3, .variationB = 3,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        /* Broken ground: it falls and piles like sand, stops the player like
           sand, and is what a laser meets when it burns through a cave-in.
           Between dirt and rock in weight, since it is both. */
        .onHeat = {true, MATERIAL_LAVA, 720.0f},
        .dynamic = true, .solid = true,
        .laserHeatRate = 1600.0f,
        .chillRate = 260.0f,
        .density = 1.9f,
    },
};

bool WorldMaterialIsSolid(CellMaterial material)
{
    return MaterialAt(material)->solid;
}

const char *WorldMaterialName(CellMaterial material)
{
    return MaterialAt(material)->name;
}

bool MaterialsValidate(void)
{
    int material;

    for (material = 0; material < MATERIAL_COUNT; ++material) {
        const MaterialInfo *info = &MATERIALS[material];

        if (info->name == NULL) {
            return false;
        }
        if (info->onHeat.enabled &&
            (info->onHeat.target < 0 || info->onHeat.target >= MATERIAL_COUNT)) {
            return false;
        }
        if (info->onCool.enabled &&
            (info->onCool.target < 0 || info->onCool.target >= MATERIAL_COUNT)) {
            return false;
        }
        /* Anything the player can stand on can also be torn loose and thrown,
           so a solid material without a mass would produce a weightless body. */
        if (info->solid && info->density <= 0.0f) {
            return false;
        }
        /* A transition that fires the instant the cell is created would make the
           material impossible to place at all. */
        if (info->onHeat.enabled &&
            info->initialTemperature >= info->onHeat.threshold) {
            return false;
        }
        if (info->onCool.enabled &&
            info->initialTemperature <= info->onCool.threshold) {
            return false;
        }
    }
    return true;
}
