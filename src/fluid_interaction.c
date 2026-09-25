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
#define FLUID_ENTER_FRACTION_IN_SPRAY 0.75f
#define FLUID_LEAVE_FRACTION 0.05f
/* How long after a low pass the water in the air around the character is
   taken for spray rather than a lake. */
#define FLUID_SPRAY_SECONDS 0.3f
/* Widest and strongest splash the character can make, in cells of radius
   and steps of push. */
#define FLUID_SPLASH_MAX_RADIUS 16.0f
#define FLUID_SPLASH_MAX_STRENGTH 18
/* The wall of water a low pass throws: cells either side of the pass, rows
   of water under the surface thrown up, steps of push and cells a tick, at
   the speeds where each starts and where each is at its greatest. Everything
   here moves cells — a pass at the sound barrier over a lake has to be a
   column of water standing in the air behind the character, not a ripple. */
#define FLUID_FLYOVER_MIN_HALF_WIDTH 8
#define FLUID_FLYOVER_MAX_HALF_WIDTH 30
#define FLUID_FLYOVER_MIN_DEPTH 3
#define FLUID_FLYOVER_MAX_DEPTH 14
#define FLUID_FLYOVER_MIN_STRENGTH 6
#define FLUID_FLYOVER_MAX_STRENGTH 48
#define FLUID_FLYOVER_MIN_PACE 1
#define FLUID_FLYOVER_MAX_PACE 4

FluidInteractionConfig FluidInteractionDefaultConfig(void)
{
    FluidInteractionConfig config;

    config.splashSpeed = 60.0f;
    config.wakeSpeed = 40.0f;
    config.flyoverHeight = 9.0f;
    config.flyoverSpeed = 90.0f;
    config.sonicFlyoverHeight = 22.0f;
    config.flyoverInterval = 0.015f;
    config.sonicBoilHeight = 5.0f;
    return config;
}

void FluidInteractionInit(FluidInteractionState *state)
{
    FluidInteractionStats empty = {0, 0, 0, 0, 0, 0};

    if (state == NULL) {
        return;
    }
    state->config = FluidInteractionDefaultConfig();
    state->submerged = 0.0f;
    state->liquid = MATERIAL_EMPTY;
    state->inside = false;
    state->flyoverCooldown = 0.0f;
    state->wakeCooldown = 0.0f;
    state->sprayTimer = 0.0f;
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
    /* Wider, deeper and faster the faster: a wall at the sound barrier, a
       ripple at a crawl. Full strength is reached at sonic speed, not boost
       speed, so the whole of a boosted pass is at full strength. */
    float pace = (speed - state->config.flyoverSpeed) /
                 (player->sonicSpeed - state->config.flyoverSpeed);
    int halfWidth;
    int depth;
    int strength;
    int cellsPerTick;
    int probe;

    if (pace < 0.0f) pace = 0.0f;
    if (pace > 1.0f) pace = 1.0f;
    halfWidth = FLUID_FLYOVER_MIN_HALF_WIDTH +
                (int)((float)(FLUID_FLYOVER_MAX_HALF_WIDTH - FLUID_FLYOVER_MIN_HALF_WIDTH) * pace);
    depth = FLUID_FLYOVER_MIN_DEPTH +
            (int)((float)(FLUID_FLYOVER_MAX_DEPTH - FLUID_FLYOVER_MIN_DEPTH) * pace);
    strength = FLUID_FLYOVER_MIN_STRENGTH +
               (int)((float)(FLUID_FLYOVER_MAX_STRENGTH - FLUID_FLYOVER_MIN_STRENGTH) * pace);
    cellsPerTick = FLUID_FLYOVER_MIN_PACE +
                   (int)((float)(FLUID_FLYOVER_MAX_PACE - FLUID_FLYOVER_MIN_PACE) * pace + 0.5f);

    for (probe = bottom + 1; probe <= bottom + (int)ceilf(reach); ++probe) {
        CellMaterial material = WorldGetCell(world, x, probe);
        int offset;
        int row;

        if (!MaterialIsLiquid(material)) {
            if (material != MATERIAL_EMPTY) {
                return;
            }
            continue;
        }
        /* The nearer the surface, the harder. */
        strength = 1 + (int)((float)strength *
                             (1.0f - 0.5f * (float)(probe - bottom - 1) / reach));
        /* A supersonic pass skimming the water boils the surface directly
           under it: the shock the drill puts through rock, put through water.
           Water only — lava is already as hot as it gets. Only the middle of
           the band, so what is boiled is a trail and not the pond. */
        if (speed >= player->sonicSpeed && material == MATERIAL_WATER &&
            (float)(probe - bottom - 1) < state->config.sonicBoilHeight) {
            int boilHalfWidth = halfWidth / 3;

            for (offset = -boilHalfWidth; offset <= boilHalfWidth; ++offset) {
                if (WorldGetCell(world, x + offset, probe) == MATERIAL_WATER) {
                    WorldSetCell(world, x + offset, probe, MATERIAL_STEAM);
                    ++state->stats.cellsBoiled;
                }
            }
        }
        /* The band under the pass, `depth` rows of it, is thrown up: the
           surface row fastest and hardest, each row under it a little less,
           and every cell of a deep row hands its push to the water above it
           on the way, so a column of water leaves the lake rather than one
           cell. Straight up under the pass, leaning outward at the edges of
           the band, so the wall curls away from the character. */
        for (row = 0; row < depth; ++row) {
            float rowShare = 1.0f - 0.6f * (float)row / (float)depth;
            int rowStrength = 1 + (int)((float)strength * rowShare);
            int rowPace = 1 + (int)((float)(cellsPerTick - 1) * rowShare + 0.5f);

            for (offset = -halfWidth; offset <= halfWidth; ++offset) {
                int directionX = abs(offset) > halfWidth / 2 ? (offset < 0 ? -1 : 1) : 0;
                int steps = 1 + (int)((float)rowStrength *
                                      (1.0f - (float)abs(offset) / (float)(halfWidth + 1)));

                if (WorldPushLiquidFast(world, x + offset, probe + row, directionX,
                                        -1, steps, rowPace)) {
                    ++state->stats.cellsPushed;
                }
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
        /* One surface per pass: the rows under it were the band. */
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
    float enterFraction;
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
    state->sprayTimer -= deltaTime;

    /* Where the character will be at the end of this frame, read now,
       before they move: at boost the frame covers six cells and the drill
       cuts the corridor first, so the water the character is about to hit
       is steam by the time they are in it, and a check of where they stand
       never saw an entry at all. */
    ahead.x = player->position.x + player->velocity.x * deltaTime;
    ahead.y = player->position.y + player->velocity.y * deltaTime;
    fractionAhead = FluidSubmergedFraction(world, player, ahead, &liquidAhead);

    /* Just after a low pass the air around the character is full of the
       water it threw, and a collider a quarter in spray is not a character
       in a lake: entry then needs most of the collider under, which a real
       dive at these speeds reaches within a frame. */
    enterFraction = state->sprayTimer > 0.0f ? FLUID_ENTER_FRACTION_IN_SPRAY
                                             : FLUID_ENTER_FRACTION;
    /* The look-ahead is for a dive: along the water, what lies ahead of a
       low pass is the spray its own wall threw forward, and flying into that
       is not going into the lake. */
    if (player->velocity.y < 0.5f * fabsf(player->velocity.x)) {
        fractionAhead = 0.0f;
    }
    if (!state->inside &&
        (fraction >= enterFraction || fractionAhead >= enterFraction)) {
        bool here = fraction >= enterFraction;

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
    /* A pass is flight along the water, not toward it: a dive is an entry,
       and the water it throws is the splash. */
    if (!state->inside && (fraction <= 0.0f || state->sprayTimer > 0.0f) &&
        speed >= state->config.flyoverSpeed && state->flyoverCooldown <= 0.0f &&
        fabsf(player->velocity.x) > fabsf(player->velocity.y)) {
        FluidFlyover(state, player, world, events, speed);
        state->sprayTimer = FLUID_SPRAY_SECONDS;
    }
}
