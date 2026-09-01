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
    /* Bounds of the solid material this blast actually removed, so a detach
       check later looks only where structure was lost. Tracked as the cells go
       rather than assumed from the radius: a blast in open air severs nothing
       and must not ask anyone to go looking. */
    int cutMinimumX = 0;
    int cutMinimumY = 0;
    int cutMaximumX = -1;
    int cutMaximumY = -1;

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
            if (MaterialIsSolid(material)) {
                if (cutMaximumX < cutMinimumX) {
                    cutMinimumX = cutMaximumX = x;
                    cutMinimumY = cutMaximumY = y;
                } else {
                    if (x < cutMinimumX) cutMinimumX = x;
                    if (x > cutMaximumX) cutMaximumX = x;
                    if (y < cutMinimumY) cutMinimumY = y;
                    if (y > cutMaximumY) cutMaximumY = y;
                }
            }
            if (material == MATERIAL_ROCK && RngRange(&world->rng, 0, 999) < chance) {
                WorldSetCellRaw(world, x, y, MATERIAL_LAVA);
            } else {
                WorldSetCellRaw(world, x, y, MATERIAL_EMPTY);
            }
        }
    }

    if (cutMaximumX >= cutMinimumX) {
        WorldRecordDestruction(world, cutMinimumX, cutMinimumY, cutMaximumX,
                               cutMaximumY);
    }
}

/* What a blow can dent: material that holds a shape. Sand and the other loose
   solids are thrown by the cone instead, which is what the force blast has
   always done with them — cratering a dune would be destroying the very thing
   the blow is supposed to send flying. */
static bool WorldPunchCanDent(CellMaterial material)
{
    return MaterialIsSolid(material) && !MaterialIsDynamic(material);
}

/* One fracture ray. Walks outward removing a thin line of solid material and
   stops the moment it leaves solid ground, so a crack never crosses open air to
   reappear somewhere else. Returns the cells removed. */
static int WorldCrackRay(World *world, Vector2 from, float angle, int length,
                         int lead, int *minimumX, int *minimumY, int *maximumX,
                         int *maximumY)
{
    float stepX = cosf(angle);
    float stepY = sinf(angle);
    int removed = 0;
    int step;
    int misses = 0;

    for (step = 1; step <= length; ++step) {
        int x = (int)floorf(from.x + stepX * (float)step);
        int y = (int)floorf(from.y + stepY * (float)step);
        int width = step * 3 < length ? 1 : 0;
        int offset;

        if (!WorldInBounds(world, x, y)) {
            break;
        }
        if (!WorldPunchCanDent(WorldMaterialAt(world, x, y))) {
            /* A gap is allowed, a chasm is not: a crack that kept going through
               open air would draw a line across the sky.

               Before the first bite the allowance is `lead` instead, because a
               crack thrown out of a crater starts inside the hole the crater
               just made and has to cross it to reach rock. Without that, every
               fracture from a blast died two cells from its own centre and the
               explosion was a plain circle after all. */
            if (++misses > (removed == 0 ? lead : 2)) {
                break;
            }
            continue;
        }
        misses = 0;
        /* Wider near the impact, hairline at the tip. */
        for (offset = -width; offset <= width; ++offset) {
            int crackX = x + (int)(-stepY * (float)offset);
            int crackY = y + (int)(stepX * (float)offset);

            if (!WorldInBounds(world, crackX, crackY) ||
                !WorldPunchCanDent(WorldMaterialAt(world, crackX, crackY))) {
                continue;
            }
            WorldSetCellRaw(world, crackX, crackY, MATERIAL_EMPTY);
            ++removed;
            if (crackX < *minimumX) *minimumX = crackX;
            if (crackX > *maximumX) *maximumX = crackX;
            if (crackY < *minimumY) *minimumY = crackY;
            if (crackY > *maximumY) *maximumY = crackY;
        }
    }
    return removed;
}

/* A star of cracks with one level of branching.

   A fracture in rock forks. A star that never forks reads as an asterisk drawn
   on the ground, which is what the punch's cracks looked like on their own, and
   forking is most of what separates the two. Everything random here is drawn
   from the world's own stream, so a replay of the same seed and inputs cracks
   the same rock the same way. */
static bool WorldFractureStar(World *world, Vector2 at, float facing, float arc,
                              int crackCount, int crackLength, int lead,
                              int *minimumX, int *minimumY, int *maximumX,
                              int *maximumY)
{
    bool cut = false;
    int crack;

    if (crackCount <= 0 || crackLength <= 0) {
        return false;
    }
    for (crack = 0; crack < crackCount; ++crack) {
        float spread = crackCount > 1
                           ? ((float)crack / (float)(crackCount - 1)) - 0.5f
                           : 0.0f;
        float jitter = (float)RngRange(&world->rng, -18, 18) * 0.0175f;
        float angle = facing + spread * arc + jitter;
        /* Uneven lengths: rock does not fail the same distance in every
           direction, and equal spokes are the other half of the asterisk. */
        int length = crackLength - RngRange(&world->rng, 0, crackLength / 2);

        if (length < 2) length = 2;
        if (WorldCrackRay(world, at, angle, length, lead, minimumX, minimumY,
                          maximumX, maximumY) > 0) {
            cut = true;
        }
        if (length >= 8) {
            float along = 0.35f +
                          (float)RngRange(&world->rng, 0, 30) * 0.01f;
            Vector2 fork = {at.x + cosf(angle) * (float)length * along,
                            at.y + sinf(angle) * (float)length * along};
            float side = RngRange(&world->rng, 0, 1) == 0 ? -1.0f : 1.0f;
            float turn = 0.35f +
                         (float)RngRange(&world->rng, 0, 40) * 0.01f;
            int reach = (int)((float)length * (1.0f - along) * 0.8f);

            if (reach >= 2 &&
                WorldCrackRay(world, fork, angle + side * turn, reach, lead,
                              minimumX, minimumY, maximumX, maximumY) > 0) {
                cut = true;
            }
        }
    }
    return cut;
}

void WorldApplyPunch(World *world, Vector2 at, Vector2 direction, int radius,
                     int crackCount, int crackLength)
{
    int centreX = (int)floorf(at.x);
    int centreY = (int)floorf(at.y);
    int minimumX = centreX;
    int minimumY = centreY;
    int maximumX = centreX;
    int maximumY = centreY;
    float facing;
    bool cut = false;
    int y;

    if (world == NULL || world->cells == NULL || radius <= 0) {
        return;
    }
    facing = atan2f(direction.y, direction.x);

    /* The bowl. Flattened along the direction of travel, so a blow that lands
       on the ground scoops a wide shallow dish rather than boring a shaft. */
    for (y = centreY - radius; y <= centreY + radius; ++y) {
        int x;

        for (x = centreX - radius; x <= centreX + radius; ++x) {
            float dx = ((float)x + 0.5f - at.x);
            float dy = ((float)y + 0.5f - at.y);
            float along = dx * cosf(facing) + dy * sinf(facing);
            float across = -dx * sinf(facing) + dy * cosf(facing);
            float shaped = along * along * 2.4f + across * across;

            if (shaped > (float)(radius * radius) ||
                !WorldInBounds(world, x, y)) {
                continue;
            }
            if (!WorldPunchCanDent(WorldMaterialAt(world, x, y))) {
                continue;
            }
            WorldSetCellRaw(world, x, y, MATERIAL_EMPTY);
            cut = true;
            if (x < minimumX) minimumX = x;
            if (x > maximumX) maximumX = x;
            if (y < minimumY) minimumY = y;
            if (y > maximumY) maximumY = y;
        }
    }

    /* Fractures out of the rim, spread around the face the blow struck. */
    if (WorldFractureStar(world, at, facing, 2.6f, crackCount, crackLength,
                          radius + 2, &minimumX, &minimumY, &maximumX,
                          &maximumY)) {
        cut = true;
    }

    if (cut) {
        WorldRecordDestruction(world, minimumX, minimumY, maximumX, maximumY);
    }
}

int WorldDrillCircle(World *world, int centerX, int centerY, int radius)
{
    int destroyed = 0;
    int radiusSquared = radius * radius;
    int rimSquared = (radius + 1) * (radius + 1);
    int y;
    /* Same reasoning as the blast: only the cells that were solid and are now
       gone. A drill boring through open air severs nothing. */
    int cutMinimumX = 0;
    int cutMinimumY = 0;
    int cutMaximumX = -1;
    int cutMaximumY = -1;

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

            if (cutMaximumX < cutMinimumX) {
                cutMinimumX = cutMaximumX = x;
                cutMinimumY = cutMaximumY = y;
            } else {
                if (x < cutMinimumX) cutMinimumX = x;
                if (x > cutMaximumX) cutMaximumX = x;
                if (y < cutMinimumY) cutMinimumY = y;
                if (y > cutMaximumY) cutMaximumY = y;
            }
            if (RngRange(&world->rng, 0, 99) < 3) {
                WorldSetCellRaw(world, x, y, MATERIAL_ASH);
            } else {
                WorldSetCellRaw(world, x, y, MATERIAL_EMPTY);
            }
            ++destroyed;
        }
    }

    if (cutMaximumX >= cutMinimumX) {
        WorldRecordDestruction(world, cutMinimumX, cutMinimumY, cutMaximumX,
                               cutMaximumY);
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
    uint16_t stamp;
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

    stamp = WorldNextEffectStamp(world);

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

void WorldApplyBlast(World *world, Vector2 at, int coreRadius,
                     float rockToLavaChance, int crackCount, int crackLength)
{
    /* Rim radii around the circle, interpolated between. Twelve is enough for a
       torn edge and few enough that the rim still reads as one crater; drawn
       once per blast from the world's stream, so the same blast on the same
       seed tears the same shape. */
    enum { BLAST_RIM_SECTORS = 12 };
    float rim[BLAST_RIM_SECTORS];
    int centreX = (int)floorf(at.x);
    int centreY = (int)floorf(at.y);
    int minimumX = centreX;
    int minimumY = centreY;
    int maximumX = centreX;
    int maximumY = centreY;
    /* How far past the torn rim the rock is left hot. Not destruction: it is
       what tells the player where the blast reached after the dust settles. */
    const float scorch = 7.0f;
    int chance = (int)(rockToLavaChance * 1000.0f);
    float reach;
    bool cut = false;
    int sector;
    int y;

    if (world == NULL || world->cells == NULL || coreRadius <= 0) {
        return;
    }
    for (sector = 0; sector < BLAST_RIM_SECTORS; ++sector) {
        rim[sector] = (float)coreRadius *
                      (0.82f + (float)RngRange(&world->rng, 0, 40) * 0.01f);
    }
    reach = (float)coreRadius * 1.22f + scorch;

    for (y = centreY - (int)reach - 1; y <= centreY + (int)reach + 1; ++y) {
        int x;

        for (x = centreX - (int)reach - 1; x <= centreX + (int)reach + 1; ++x) {
            float dx = (float)x + 0.5f - at.x;
            float dy = (float)y + 0.5f - at.y;
            float distance = sqrtf(dx * dx + dy * dy);
            float turn;
            float slot;
            int low;
            int high;
            float blend;
            float edge;
            Cell *cell;

            if (distance > reach || !WorldInBounds(world, x, y)) {
                continue;
            }
            /* The rim radius for this direction, blended between neighbouring
               sectors so the edge waves rather than steps. */
            turn = atan2f(dy, dx);
            slot = (turn + PI) / (2.0f * PI) * (float)BLAST_RIM_SECTORS;
            low = ((int)floorf(slot)) % BLAST_RIM_SECTORS;
            if (low < 0) low += BLAST_RIM_SECTORS;
            high = (low + 1) % BLAST_RIM_SECTORS;
            blend = slot - floorf(slot);
            edge = rim[low] + (rim[high] - rim[low]) * blend;

            if (distance <= edge) {
                if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) {
                    cut = true;
                    if (x < minimumX) minimumX = x;
                    if (x > maximumX) maximumX = x;
                    if (y < minimumY) minimumY = y;
                    if (y > maximumY) maximumY = y;
                }
                if (WorldMaterialAt(world, x, y) == MATERIAL_ROCK &&
                    RngRange(&world->rng, 0, 999) < chance) {
                    WorldSetCellRaw(world, x, y, MATERIAL_LAVA);
                } else {
                    WorldSetCellRaw(world, x, y, MATERIAL_EMPTY);
                }
                continue;
            }
            if (distance > edge + scorch) {
                continue;
            }
            /* Scorched, not destroyed. The heat falls off to nothing at the
               outer edge, so the crater has a glowing lip that cools rather
               than a second sharp ring. Rock melts at 720; the peak here is
               well under that, so a blast leaves a mark and does not start a
               lava field. */
            cell = WorldCell(world, x, y);
            if (cell->material == MATERIAL_EMPTY) {
                continue;
            }
            cell->temperature += 380.0f * (1.0f - (distance - edge) / scorch);
            WorldWakeCellAndNeighbors(world, x, y);
            (void)WorldTryThermalTransition(world, x, y);
        }
    }

    /* And the fractures, all the way round. These are what carry the blast past
       its own radius: the rock beyond the crater is left broken rather than
       untouched, so a second shot into the same face finds ground that already
       gave way once. */
    if (WorldFractureStar(world, at, 0.0f, 2.0f * PI, crackCount, crackLength,
                          coreRadius + 4, &minimumX, &minimumY, &maximumX,
                          &maximumY)) {
        cut = true;
    }

    if (cut) {
        WorldRecordDestruction(world, minimumX, minimumY, maximumX, maximumY);
    }
}

void WorldApplyShockwave(World *world, int centerX, int centerY, int innerRadius,
                         int outerRadius)
{
    uint16_t stamp;
    int band;

    if (world == NULL || world->cells == NULL || innerRadius < 0 ||
        outerRadius <= innerRadius) {
        return;
    }

    stamp = WorldNextEffectStamp(world);

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
    uint16_t stamp;
    LaserResult result = {end, MATERIAL_EMPTY, false};

    if (world == NULL || world->cells == NULL || length < 0.001f) {
        result.position = start;
        return result;
    }

    stamp = WorldNextEffectStamp(world);

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

LaserResult WorldBeamHit(const World *world, Vector2 start, Vector2 end)
{
    Vector2 delta = {end.x - start.x, end.y - start.y};
    float length = sqrtf(delta.x * delta.x + delta.y * delta.y);
    int steps = (int)ceilf(length / 0.65f);
    int step;
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
    return result;
}

LaserResult WorldApplyLaser(World *world, Vector2 start, Vector2 end, float radius,
                            float deltaTime)
{
    LaserResult result = WorldBeamHit(world, start, end);
    uint16_t stamp;

    if (world == NULL || world->cells == NULL || !result.hit) {
        return result;
    }

    stamp = WorldNextEffectStamp(world);

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
