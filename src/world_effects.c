/* Powers acting on the world: drilling, explosions, the force blast, the laser
 * and the cryo beam.
 *
 * These are the world-side half of the abilities; presentation and cooldowns
 * live in powers.c. Two rules recur and are worth knowing before editing any of
 * them: an effect that moves cells visits them from the far edge inwards and
 * stamps each one with `effectStamp`, so a cell thrown outward cannot be picked
 * up again by the same blow; and an effect that reaches across space must
 * respect what stands in the way, because a blow that scours the far side of a
 * wall reads as the power passing straight through the world.
 */
#include "world_internal.h"

#include <math.h>

#include <raymath.h>

#include "world_thermal.h"

void WorldDestroyCircle(World *world, int centerX, int centerY, int radius,
                        float rockToLavaChance)
{
    int x;
    int y;
    int radiusSquared = radius * radius;
    int chance = (int)(Clamp(rockToLavaChance, 0.0f, 1.0f) * 1000.0f);

    if (world == NULL || world->cells == NULL) {
        return;
    }

    for (y = centerY - radius; y <= centerY + radius; ++y) {
        for (x = centerX - radius; x <= centerX + radius; ++x) {
            int dx = x - centerX;
            int dy = y - centerY;
            CellMaterial material;

            if (dx * dx + dy * dy > radiusSquared || !WorldInBounds(world, x, y)) {
                continue;
            }

            material = WorldMaterialAt(world, x, y);
            if (material == MATERIAL_ROCK && RngRange(&world->rng, 0, 999) < chance) {
                WorldSetCellRaw(world, x, y, MATERIAL_LAVA);
            } else {
                WorldSetCellRaw(world, x, y, MATERIAL_EMPTY);
            }
        }
    }
}

int WorldDrillCircle(World *world, int centerX, int centerY, int radius)
{
    int destroyed = 0;
    int radiusSquared = radius * radius;
    int rimSquared = (radius + 1) * (radius + 1);
    int y;

    if (world == NULL || world->cells == NULL || radius < 0) {
        return 0;
    }

    for (y = centerY - radius - 1; y <= centerY + radius + 1; ++y) {
        int x;

        for (x = centerX - radius - 1; x <= centerX + radius + 1; ++x) {
            int dx = x - centerX;
            int dy = y - centerY;
            int distanceSquared = dx * dx + dy * dy;
            Cell *cell;

            if (distanceSquared > rimSquared || !WorldInBounds(world, x, y)) {
                continue;
            }

            cell = WorldCell(world, x, y);
            if (distanceSquared > radiusSquared ||
                !MaterialIsSolid(cell->material)) {
                /* Everything the drill cannot cut is only warmed. The cap stays
                   under every phase threshold, so a tunnel can never ignite
                   dirt or boil water on its own. */
                if (cell->material != MATERIAL_EMPTY &&
                    cell->temperature < DRILL_WALL_TEMPERATURE) {
                    cell->temperature = DRILL_WALL_TEMPERATURE;
                    WorldWakeCellAndNeighbors(world, x, y);
                }
                continue;
            }

            if (RngRange(&world->rng, 0, 99) < 3) {
                WorldSetCellRaw(world, x, y, MATERIAL_ASH);
            } else {
                WorldSetCellRaw(world, x, y, MATERIAL_EMPTY);
            }
            ++destroyed;
        }
    }

    return destroyed;
}

/* One heavy blow along a cone. Loose material is thrown a long way; solid
   material is not moved but is scoured, a thin layer of its exposed face turning
   to ash, so the blast leaves a visible mark where it landed and then blows the
   dust it just made downwind.
 *
 * Cells are visited from the far edge of the cone inwards so a cell thrown
 * outward cannot be picked up again by the same blow, and `effectStamp` makes
 * that guarantee exact.
 */
/* Angular resolution of the occlusion pre-pass. At the configured cone's far
   edge the arc is about one hundred cells across, so this is still finer than
   one ray per cell there. */
#define FORCE_BLAST_RAYS 160
void WorldApplyForceBlast(World *world, Vector2 origin, Vector2 direction,
                          float length, float spreadCosine, int reach)
{
    float blocked[FORCE_BLAST_RAYS];
    float centreAngle;
    float halfSpread;
    uint32_t stamp;
    int ray;
    int step;

    if (world == NULL || world->cells == NULL || length <= 0.0f || reach <= 0) {
        return;
    }

    /* A blow does not reach round a corner. Without this the cone shoves sand
       on the far side of a rock wall and scours the wall's back face, which
       reads as the blast passing straight through the world. One cheap ray per
       angular slice records where the cone first meets something solid; the cell
       pass then refuses to touch anything further along that slice. */
    centreAngle = atan2f(direction.y, direction.x);
    halfSpread = acosf(Clamp(spreadCosine, -1.0f, 1.0f));
    for (ray = 0; ray < FORCE_BLAST_RAYS; ++ray) {
        float angle = centreAngle - halfSpread +
                      2.0f * halfSpread * ((float)ray / (float)(FORCE_BLAST_RAYS - 1));
        float rayX = cosf(angle);
        float rayY = sinf(angle);
        float travelled;

        blocked[ray] = length;
        for (travelled = 1.0f; travelled <= length; travelled += 0.5f) {
            int sampleX = (int)floorf(origin.x + rayX * travelled);
            int sampleY = (int)floorf(origin.y + rayY * travelled);

            if (MaterialIsSolid(WorldMaterialAt(world, sampleX, sampleY))) {
                blocked[ray] = travelled;
                break;
            }
        }
    }

    stamp = ++world->effectSerial;
    if (stamp == 0u) {
        stamp = ++world->effectSerial;
    }

    for (step = (int)length; step >= 1; --step) {
        int extent = step + 1;
        int offsetY;

        for (offsetY = -extent; offsetY <= extent; ++offsetY) {
            int offsetX;

            for (offsetX = -extent; offsetX <= extent; ++offsetX) {
                float dx = (float)offsetX;
                float dy = (float)offsetY;
                float distance = sqrtf(dx * dx + dy * dy);
                int x;
                int y;
                float strength;
                int push;
                Cell *cell;

                /* One shell of the cone per step, so the ring order above holds. */
                if (distance < (float)step - 0.5f || distance >= (float)step + 0.5f) {
                    continue;
                }
                if (dx * direction.x + dy * direction.y < distance * spreadCosine) {
                    continue;
                }

                {
                    /* Which angular slice this cell sits in, and whether the
                       blast still reaches that far along it. The face that
                       blocked the ray is itself included, so the wall the blow
                       lands on is marked. */
                    float offset = atan2f(dy, dx) - centreAngle;
                    int slice;

                    while (offset > PI) offset -= 2.0f * PI;
                    while (offset < -PI) offset += 2.0f * PI;
                    slice = (int)roundf((offset + halfSpread) /
                                        (2.0f * halfSpread) *
                                        (float)(FORCE_BLAST_RAYS - 1));
                    slice = slice < 0 ? 0
                                      : (slice >= FORCE_BLAST_RAYS
                                             ? FORCE_BLAST_RAYS - 1
                                             : slice);
                    if (distance > blocked[slice] + 1.5f) {
                        continue;
                    }
                }

                x = (int)floorf(origin.x) + offsetX;
                y = (int)floorf(origin.y) + offsetY;
                if (!WorldInBounds(world, x, y)) {
                    continue;
                }
                cell = WorldCell(world, x, y);
                if (cell->material == MATERIAL_EMPTY || cell->effectStamp == stamp) {
                    continue;
                }
                strength = 1.0f - distance / length;
                if (strength <= 0.0f) {
                    continue;
                }
                cell->effectStamp = stamp;

                if (!MaterialIsDynamic(cell->material)) {
                    /* Static terrain holds, but the face that took the blow is
                       scoured to dust. Only cells that are actually exposed are
                       marked, so the dent follows the shape of the surface
                       instead of hollowing out the inside of a hill. */
                    int aheadX = x + (int)roundf(direction.x);
                    int aheadY = y + (int)roundf(direction.y);
                    int behindX = x - (int)roundf(direction.x);
                    int behindY = y - (int)roundf(direction.y);

                    if (!MaterialIsSolid(cell->material) ||
                        (MaterialIsSolid(WorldMaterialAt(world, aheadX, aheadY)) &&
                         MaterialIsSolid(WorldMaterialAt(world, behindX, behindY)))) {
                        continue;
                    }
                    /* A central hit now has enough bite to leave a visible dent
                       in one or two presses. The exposure check above still
                       limits this to the face, so more power cannot hollow the
                       hill out behind its surface. */
                    if ((float)RngRange(&world->rng, 0, 999) < strength * 160.0f) {
                        WorldSetCellRaw(world, x, y, MATERIAL_ASH);
                    }
                    continue;
                }

                /* Linear rather than squared falloff: squaring leaves anything
                   past the first few cells barely moving, which reads as a weak
                   blow however large the numbers are. */
                push = 2 + (int)(strength * (float)reach);
                for (; push >= 1; --push) {
                    int targetX = (int)roundf((float)x + direction.x * (float)push);
                    int targetY = (int)roundf((float)y + direction.y * (float)push);

                    if ((targetX != x || targetY != y) &&
                        WorldInBounds(world, targetX, targetY) &&
                        WorldMaterialAt(world, targetX, targetY) == MATERIAL_EMPTY) {
                        WorldMoveCell(world, x, y, targetX, targetY);
                        break;
                    }
                }
            }
        }
    }
}

void WorldApplyShockwave(World *world, int centerX, int centerY, int innerRadius,
                         int outerRadius)
{
    uint32_t stamp;
    int band;

    if (world == NULL || world->cells == NULL || innerRadius < 0 ||
        outerRadius <= innerRadius) {
        return;
    }

    stamp = ++world->effectSerial;
    if (stamp == 0u) {
        stamp = ++world->effectSerial;
    }

    /* Outer bands move first, so displaced cells cannot be pushed twice. */
    for (band = outerRadius; band > innerRadius; --band) {
        int y;

        for (y = centerY - band; y <= centerY + band; ++y) {
            int x;

            for (x = centerX - band; x <= centerX + band; ++x) {
                float dx = (float)(x - centerX);
                float dy = (float)(y - centerY);
                float distanceSquared = dx * dx + dy * dy;
                float distance;
                float directionX;
                float directionY;
                float strength;
                int pushDistance;
                int push;
                Cell *cell;

                if (!WorldInBounds(world, x, y) ||
                    distanceSquared > (float)(band * band) ||
                    distanceSquared <= (float)((band - 1) * (band - 1))) {
                    continue;
                }

                cell = WorldCell(world, x, y);
                if (!MaterialIsDynamic(cell->material) || cell->effectStamp == stamp) {
                    continue;
                }

                distance = sqrtf(distanceSquared);
                directionX = dx / distance;
                directionY = dy / distance;
                strength = 1.0f - (distance - (float)innerRadius) /
                                      (float)(outerRadius - innerRadius);
                pushDistance = 2 + (int)(Clamp(strength, 0.0f, 1.0f) * 10.0f);
                cell->effectStamp = stamp;

                for (push = pushDistance; push >= 1; --push) {
                    int targetX = (int)roundf((float)x + directionX * (float)push);
                    int targetY = (int)roundf((float)y + directionY * (float)push);

                    if ((targetX != x || targetY != y) &&
                        WorldInBounds(world, targetX, targetY) &&
                        WorldMaterialAt(world, targetX, targetY) == MATERIAL_EMPTY) {
                        WorldMoveCell(world, x, y, targetX, targetY);
                        break;
                    }
                }
            }
        }
    }
}

/* The thermal inverse of the laser. Every other power removes matter; this one
   changes its phase, which is the only way the player can put something into the
   world instead of taking it out. It passes through what the laser passes
   through and chills everything on the way, so sweeping a pond freezes its
   surface and holding it on lava turns a lake back into rock. */
LaserResult WorldApplyChill(World *world, Vector2 start, Vector2 end, float radius,
                            float deltaTime)
{
    Vector2 delta = {end.x - start.x, end.y - start.y};
    float length = sqrtf(delta.x * delta.x + delta.y * delta.y);
    int steps = (int)ceilf(length / 0.65f);
    int step;
    uint32_t stamp;
    LaserResult result = {end, MATERIAL_EMPTY, false};

    if (world == NULL || world->cells == NULL || length < 0.001f) {
        result.position = start;
        return result;
    }

    stamp = ++world->effectSerial;
    if (stamp == 0u) {
        stamp = ++world->effectSerial;
    }

    for (step = 0; step <= steps; ++step) {
        float amount = (float)step / (float)steps;
        Vector2 point = {start.x + delta.x * amount, start.y + delta.y * amount};
        int centerX = (int)floorf(point.x);
        int centerY = (int)floorf(point.y);
        int brush = (int)ceilf(radius);
        CellMaterial blocking;
        int y;

        if (!WorldInBounds(world, centerX, centerY)) {
            break;
        }

        for (y = centerY - brush; y <= centerY + brush; ++y) {
            int x;

            for (x = centerX - brush; x <= centerX + brush; ++x) {
                int dx = x - centerX;
                int dy = y - centerY;
                float rate;
                Cell *cell;

                if ((float)(dx * dx + dy * dy) > radius * radius ||
                    !WorldInBounds(world, x, y)) {
                    continue;
                }
                cell = WorldCell(world, x, y);
                if (cell->effectStamp == stamp || cell->material == MATERIAL_EMPTY) {
                    continue;
                }
                cell->effectStamp = stamp;

                rate = MaterialAt(cell->material)->chillRate;
                cell->temperature -= deltaTime * rate;
                WorldWakeCellAndNeighbors(world, x, y);
                (void)WorldTryThermalTransition(world, x, y);
            }
        }

        blocking = WorldMaterialAt(world, centerX, centerY);
        if (MaterialIsSolid(blocking)) {
            result.position = point;
            result.material = blocking;
            result.hit = true;
            break;
        }
    }

    return result;
}

LaserResult WorldApplyLaser(World *world, Vector2 start, Vector2 end, float radius,
                            float deltaTime)
{
    Vector2 delta = {end.x - start.x, end.y - start.y};
    float length = sqrtf(delta.x * delta.x + delta.y * delta.y);
    int steps = (int)ceilf(length / 0.65f);
    int step;
    uint32_t stamp;
    LaserResult result = {end, MATERIAL_EMPTY, false};

    if (world == NULL || world->cells == NULL || length < 0.001f) {
        result.position = start;
        return result;
    }

    for (step = 0; step <= steps; ++step) {
        float amount = (float)step / (float)steps;
        int centerX = (int)floorf(start.x + delta.x * amount);
        int centerY = (int)floorf(start.y + delta.y * amount);
        CellMaterial material;

        if (!WorldInBounds(world, centerX, centerY)) {
            break;
        }
        material = WorldMaterialAt(world, centerX, centerY);
        if (MaterialIsSolid(material)) {
            result.position = (Vector2){start.x + delta.x * amount,
                                        start.y + delta.y * amount};
            result.material = material;
            result.hit = true;
            break;
        }
    }

    if (!result.hit) {
        return result;
    }

    stamp = ++world->effectSerial;
    if (stamp == 0u) {
        stamp = ++world->effectSerial;
    }

    {
        int centerX = (int)floorf(result.position.x);
        int centerY = (int)floorf(result.position.y);
        int brush = (int)ceilf(radius);
        int y;

        for (y = centerY - brush; y <= centerY + brush; ++y) {
            int x;

            for (x = centerX - brush; x <= centerX + brush; ++x) {
                int dx = x - centerX;
                int dy = y - centerY;
                float rate;
                Cell *cell;

                if ((float)(dx * dx + dy * dy) > radius * radius ||
                    !WorldInBounds(world, x, y)) {
                    continue;
                }

                cell = WorldCell(world, x, y);
                if (cell->effectStamp == stamp) {
                    continue;
                }
                cell->effectStamp = stamp;

                rate = MaterialAt(cell->material)->laserHeatRate;
                if (rate > 0.0f) {
                    cell->temperature += deltaTime * rate;
                    WorldWakeCellAndNeighbors(world, x, y);
                    (void)WorldTryThermalTransition(world, x, y);
                }
            }
        }
    }

    return result;
}
