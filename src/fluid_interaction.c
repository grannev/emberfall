/* The player and a liquid. See fluid_interaction.h for the shape of it; this
 * file records the decisions inside.
 */
#include "fluid_interaction.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#include "materials.h"

/* Samples through the collider: three columns by three rows. Nine reads a
   frame, and enough to tell a character standing in the shallows from one
   that has gone under. */
#define FLUID_SAMPLE_COLUMNS 3
#define FLUID_SAMPLE_ROWS 3
/* Hysteresis on "in the liquid". */
#define FLUID_ENTER_FRACTION 0.25f
#define FLUID_LEAVE_FRACTION 0.05f
/* Widest and strongest splash the character can make, in cells of radius
   and steps of push. */
#define FLUID_SPLASH_MAX_RADIUS 16.0f
#define FLUID_SPLASH_MAX_STRENGTH 18
/* The wall of water a low pass throws: cells either side of the pass, and
   steps of push, at the speeds where each starts and where each is at its
   widest. */
#define FLUID_FLYOVER_MIN_HALF_WIDTH 4
#define FLUID_FLYOVER_MAX_HALF_WIDTH 16
#define FLUID_FLYOVER_MAX_STRENGTH 16

FluidInteractionConfig FluidInteractionDefaultConfig(void)
{
    FluidInteractionConfig config;

    config.splashSpeed = 60.0f;
    config.wakeSpeed = 40.0f;
    config.flyoverHeight = 6.0f;
    config.flyoverSpeed = 110.0f;
    config.sonicFlyoverHeight = 14.0f;
    config.flyoverInterval = 0.03f;
    return config;
}

void FluidInteractionInit(FluidInteractionState *state)
{
    FluidInteractionStats empty = {0, 0, 0, 0, 0};

    if (state == NULL) {
        return;
    }
    state->config = FluidInteractionDefaultConfig();
    state->submerged = 0.0f;
    state->liquid = MATERIAL_EMPTY;
    state->inside = false;
    state->flyoverCooldown = 0.0f;
    state->wakeCooldown = 0.0f;
    state->stats = empty;
}

/* Fraction of the collider's samples that are liquid, and which liquid the
   most of them are. */
static float FluidSubmergedFraction(const World *world, const Player *player,
                                    Vector2 at, CellMaterial *liquid)
{
    int counts[MATERIAL_COUNT] = {0};
    int total = 0;
    int inLiquid = 0;
    int column;
    int row;
    int best = MATERIAL_EMPTY;

    for (row = 0; row < FLUID_SAMPLE_ROWS; ++row) {
        float y = at.y - player->radius +
                  2.0f * player->radius * (float)row / (float)(FLUID_SAMPLE_ROWS - 1);

        for (column = 0; column < FLUID_SAMPLE_COLUMNS; ++column) {
            float x = at.x - player->radius +
                      2.0f * player->radius * (float)column /
                          (float)(FLUID_SAMPLE_COLUMNS - 1);
            CellMaterial material = WorldGetCell(world, (int)floorf(x),
                                                 (int)floorf(y));

            ++total;
            if (MaterialIsLiquid(material)) {
                ++inLiquid;
                ++counts[material];
                if (counts[material] > counts[best]) {
                    best = (int)material;
                }
            }
        }
    }
    *liquid = (CellMaterial)best;
    return total > 0 ? (float)inLiquid / (float)total : 0.0f;
}

/* The row of the surface of the liquid the character is in: the topmost
   liquid cell in their column within reach of the collider, or the
   character's own row when the column is not liquid at all. */
static int FluidSurfaceRow(const World *world, const Player *player, Vector2 at)
{
    int x = (int)floorf(at.x);
    int y = (int)floorf(at.y);
    int reach = (int)ceilf(player->radius) + 6;
    int probe;
    int surface = y;

    for (probe = y + reach; probe >= y - reach; --probe) {
        if (MaterialIsLiquid(WorldGetCell(world, x, probe))) {
            surface = probe;
        }
    }
    return surface;
}

static void FluidSplash(FluidInteractionState *state, const Player *player,
                        World *world, GameEventBuffer *events, float speed,
                        Vector2 where, bool entering)
{
    Vector2 at = {where.x, (float)FluidSurfaceRow(world, player, where) + 0.5f};
    float radius = 4.0f + speed / 30.0f;
    int strength = 4 + (int)(speed / 25.0f);
    Vector2 direction = {0.0f, entering ? 1.0f : -1.0f};

    if (radius > FLUID_SPLASH_MAX_RADIUS) radius = FLUID_SPLASH_MAX_RADIUS;
    if (strength > FLUID_SPLASH_MAX_STRENGTH) strength = FLUID_SPLASH_MAX_STRENGTH;
    if (speed > 0.0f) {
        direction.x = player->velocity.x / speed;
        direction.y = player->velocity.y / speed;
    }
    state->stats.cellsPushed += WorldSplashLiquid(world, at, radius, strength);
    (void)GameEventsPush(events, (GameEvent){
        .type = GAME_EVENT_LIQUID_SPLASH,
        .position = at,
        .direction = direction,
        .strength = speed,
        .radius = radius,
        .material = state->liquid,
        .count = 0,
    });
}

/* Under water at speed: the cells around the character are thrown out of
   the way, ahead and to the sides, and rise behind as the wake. */
static void FluidWake(FluidInteractionState *state, const Player *player,
                      World *world, float speed)
{
    float radius = player->radius + 2.0f + speed / 60.0f;
    int strength = 2 + (int)(speed / 40.0f);

    if (radius > 10.0f) radius = 10.0f;
    if (strength > 8) strength = 8;
    state->stats.cellsPushed += WorldPushLiquidRadial(world, player->position,
                                                      radius, strength);
    ++state->stats.wakes;
}

static void FluidFlyover(FluidInteractionState *state, const Player *player,
                         World *world, GameEventBuffer *events, float speed)
{
    int x = (int)floorf(player->position.x);
    int bottom = (int)floorf(player->position.y + player->radius);
    float reach = speed >= player->sonicSpeed ? state->config.sonicFlyoverHeight
                                              : state->config.flyoverHeight;
    /* Wider and higher the faster: a wall at boost, a ripple at a crawl. */
    float pace = (speed - state->config.flyoverSpeed) /
                 (player->boostSpeed - state->config.flyoverSpeed);
    int halfWidth;
    int strength;
    int probe;

    if (pace < 0.0f) pace = 0.0f;
    if (pace > 1.0f) pace = 1.0f;
    halfWidth = FLUID_FLYOVER_MIN_HALF_WIDTH +
                (int)((float)(FLUID_FLYOVER_MAX_HALF_WIDTH - FLUID_FLYOVER_MIN_HALF_WIDTH) * pace);
    strength = 2 + (int)((float)(FLUID_FLYOVER_MAX_STRENGTH - 2) * pace);

    for (probe = bottom + 1; probe <= bottom + (int)ceilf(reach); ++probe) {
        CellMaterial material = WorldGetCell(world, x, probe);
        int offset;

        if (!MaterialIsLiquid(material)) {
            if (material != MATERIAL_EMPTY) {
                return;
            }
            continue;
        }
        /* The nearer the surface, the harder. Straight up under the pass,
           leaning outward at the edges of the band, so the wall curls. */
        strength = 1 + (int)((float)strength *
                             (1.0f - 0.5f * (float)(probe - bottom - 1) / reach));
        for (offset = -halfWidth; offset <= halfWidth; ++offset) {
            int directionX = abs(offset) > halfWidth / 2 ? (offset < 0 ? -1 : 1) : 0;
            int steps = 1 + (int)((float)strength *
                                  (1.0f - (float)abs(offset) / (float)(halfWidth + 1)));

            if (WorldPushLiquid(world, x + offset, probe, directionX, -1, steps)) {
                ++state->stats.cellsPushed;
            }
        }
        ++state->stats.flyovers;
        state->flyoverCooldown = state->config.flyoverInterval;
        (void)GameEventsPush(events, (GameEvent){
            .type = GAME_EVENT_LIQUID_RIPPLE,
            .position = {(float)x + 0.5f, (float)probe},
            .direction = {player->velocity.x / speed, player->velocity.y / speed},
            .strength = speed,
            .radius = (float)(2 * halfWidth + 1),
            .material = material,
        });
        return;
    }
}

void FluidInteractionUpdatePlayer(FluidInteractionState *state,
                                  const Player *player, World *world,
                                  GameEventBuffer *events, float deltaTime)
{
    float speed;
    float fraction;
    float fractionAhead;
    CellMaterial liquid;
    CellMaterial liquidAhead;
    Vector2 ahead;

    if (state == NULL || player == NULL || world == NULL || world->cells == NULL ||
        events == NULL) {
        return;
    }
    speed = sqrtf(player->velocity.x * player->velocity.x +
                  player->velocity.y * player->velocity.y);
    fraction = FluidSubmergedFraction(world, player, player->position, &liquid);
    state->submerged = fraction;
    if (fraction > 0.0f) {
        state->liquid = liquid;
    }
    state->flyoverCooldown -= deltaTime;
    state->wakeCooldown -= deltaTime;

    /* Where the character will be at the end of this frame, read now,
       before they move: at boost the frame covers six cells and the drill
       cuts the corridor first, so the water the character is about to hit
       is steam by the time they are in it, and a check of where they stand
       never saw an entry at all. */
    ahead.x = player->position.x + player->velocity.x * deltaTime;
    ahead.y = player->position.y + player->velocity.y * deltaTime;
    fractionAhead = FluidSubmergedFraction(world, player, ahead, &liquidAhead);

    if (!state->inside &&
        (fraction >= FLUID_ENTER_FRACTION || fractionAhead >= FLUID_ENTER_FRACTION)) {
        bool here = fraction >= FLUID_ENTER_FRACTION;

        state->inside = true;
        ++state->stats.entries;
        if (!here) {
            state->liquid = liquidAhead;
        }
        if (speed >= state->config.splashSpeed) {
            FluidSplash(state, player, world, events, speed,
                        here ? player->position : ahead, true);
        }
        return;
    }
    if (state->inside && fraction <= FLUID_LEAVE_FRACTION &&
        fractionAhead <= FLUID_LEAVE_FRACTION) {
        state->inside = false;
        ++state->stats.exits;
        if (speed >= state->config.splashSpeed) {
            FluidSplash(state, player, world, events, speed, player->position, false);
        }
        return;
    }
    if (state->inside && fraction > 0.0f && speed >= state->config.wakeSpeed &&
        !PlayerIsDrilling(player) && state->wakeCooldown <= 0.0f) {
        /* Not while drilling: the drill is turning the water to steam, and
           steam is not shoved. */
        FluidWake(state, player, world, speed);
        state->wakeCooldown = 0.05f;
        return;
    }
    if (!state->inside && fraction <= 0.0f && speed >= state->config.flyoverSpeed &&
        state->flyoverCooldown <= 0.0f) {
        FluidFlyover(state, player, world, events, speed);
    }
}
