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
        .variationR = 2, .variationG = 1,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .onHeat = {true, MATERIAL_FIRE, 175.0f},
        .solid = true,
        .laserHeatRate = 2500.0f,
        .chillRate = 260.0f,
        .density = 1.4f,
    },
    [MATERIAL_ROCK] = {
        .name = "ROCK", .color = {72, 77, 86, 255},
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .onHeat = {true, MATERIAL_LAVA, 720.0f},
        .solid = true,
        .laserHeatRate = 1080.0f,
        .chillRate = 260.0f,
        .density = 2.6f,
    },
    [MATERIAL_SAND] = {
        .name = "SAND", .color = {218, 184, 91, 255},
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
        .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .onHeat = {true, MATERIAL_STEAM, 108.0f},
        .onCool = {true, MATERIAL_ICE, -4.0f},
        .dynamic = true,
        .chillRate = 90.0f,
        .density = 1.0f,
    },
    [MATERIAL_LAVA] = {
        .name = "LAVA", .color = {245, 73, 18, 255},
        .variationG = 4,
        .initialTemperature = 900.0f,
        .selfHeatTarget = 900.0f, .selfHeatRate = 0.08f,
                /* Lava will not cool this far on its own — it relaxes back toward 900 —
           so this threshold only ever fires under the cryo beam. */
        .onCool = {true, MATERIAL_ROCK, 620.0f},
        .dynamic = true,
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
        .variationR = 2, .variationG = 2,
        .initialTemperature = 125.0f,
        .linearCoolRate = 0.42f,
        .onCool = {true, MATERIAL_WATER, 58.0f},
        .dynamic = true,
        .chillRate = 320.0f,
    },
    [MATERIAL_SMOKE] = {
        .name = "SMOKE", .color = {83, 88, 94, 205},
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = 75.0f,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .dynamic = true,
        .chillRate = 260.0f,
    },
    [MATERIAL_FIRE] = {
        .name = "FIRE", .color = {255, 132, 24, 245},
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
    },
    [MATERIAL_ASH] = {
        .name = "ASH", .color = {112, 108, 104, 255},
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .dynamic = true,
        .chillRate = 260.0f,
        .density = 0.7f,
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
