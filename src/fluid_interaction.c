/* The player and a liquid. See fluid_interaction.h for the shape of it; this
 * file records the decisions inside.
 */
#include "fluid_interaction.h"

#include <math.h>
#include <stddef.h>

#include "materials.h"

/* Samples through the collider: three columns by three rows. Nine reads a
   frame, and enough to tell a character standing in the shallows from one
   that has gone under. */
#define FLUID_SAMPLE_COLUMNS 3
#define FLUID_SAMPLE_ROWS 3
/* Hysteresis on "in the liquid". */
#define FLUID_ENTER_FRACTION 0.25f
#define FLUID_LEAVE_FRACTION 0.05f
/* Widest and strongest splash the character can make, in cells of push
   radius and steps of push. */
#define FLUID_SPLASH_MAX_RADIUS 10.0f
#define FLUID_SPLASH_MAX_STRENGTH 10
/* Cells lifted either side of a low pass. */
#define FLUID_FLYOVER_HALF_WIDTH 2

FluidInteractionConfig FluidInteractionDefaultConfig(void)
{
    FluidInteractionConfig config;

    config.dragCoefficient = 0.028f;
    config.lavaDragScale = 2.0f;
    config.splashSpeed = 60.0f;
    config.entryLoss = 0.30f;
    config.flyoverHeight = 4.0f;
    config.flyoverSpeed = 120.0f;
    config.flyoverInterval = 0.05f;
    return config;
}

void FluidInteractionInit(FluidInteractionState *state)
{
    FluidInteractionStats empty = {0, 0, 0, 0};

    if (state == NULL) {
        return;
    }
    state->config = FluidInteractionDefaultConfig();
    state->submerged = 0.0f;
    state->liquid = MATERIAL_EMPTY;
    state->inside = false;
    state->flyoverCooldown = 0.0f;
    state->stats = empty;
}

/* Fraction of the collider's samples that are liquid, and which liquid the
   most of them are. */
static float FluidSubmergedFraction(const World *world, const Player *player,
                                    CellMaterial *liquid)
{
    int counts[MATERIAL_COUNT] = {0};
    int total = 0;
    int inLiquid = 0;
    int column;
    int row;
    int best = MATERIAL_EMPTY;

    for (row = 0; row < FLUID_SAMPLE_ROWS; ++row) {
        float y = player->position.y - player->radius +
                  2.0f * player->radius * (float)row / (float)(FLUID_SAMPLE_ROWS - 1);

        for (column = 0; column < FLUID_SAMPLE_COLUMNS; ++column) {
            float x = player->position.x - player->radius +
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
static int FluidSurfaceRow(const World *world, const Player *player)
{
    int x = (int)floorf(player->position.x);
    int y = (int)floorf(player->position.y);
    int reach = (int)ceilf(player->radius) + 4;
    int probe;
    int surface = y;

    for (probe = y + reach; probe >= y - reach; --probe) {
        if (MaterialIsLiquid(WorldGetCell(world, x, probe))) {
            surface = probe;
        }
    }
    return surface;
}

static void FluidSplash(FluidInteractionState *state, Player *player,
                        World *world, GameEventBuffer *events, float speed,
                        bool entering)
{
    Vector2 at = {player->position.x, (float)FluidSurfaceRow(world, player) + 0.5f};
    float radius = 3.0f + speed / 60.0f;
    int strength = 2 + (int)(speed / 40.0f);
    Vector2 direction = {0.0f, entering ? 1.0f : -1.0f};

    if (radius > FLUID_SPLASH_MAX_RADIUS) radius = FLUID_SPLASH_MAX_RADIUS;
    if (strength > FLUID_SPLASH_MAX_STRENGTH) strength = FLUID_SPLASH_MAX_STRENGTH;
    if (speed > 0.0f) {
        direction.x = player->velocity.x / speed;
        direction.y = player->velocity.y / speed;
    }
    state->stats.cellsPushed += WorldPushLiquidRadial(world, at, radius, strength);
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

static void FluidFlyover(FluidInteractionState *state, const Player *player,
                         World *world, GameEventBuffer *events, float speed)
{
    int x = (int)floorf(player->position.x);
    int bottom = (int)floorf(player->position.y + player->radius);
    int probe;

    for (probe = bottom + 1;
         probe <= bottom + (int)ceilf(state->config.flyoverHeight); ++probe) {
        CellMaterial material = WorldGetCell(world, x, probe);
        int offset;
        int strength;

        if (!MaterialIsLiquid(material)) {
            if (material != MATERIAL_EMPTY) {
                return;
            }
            continue;
        }
        /* The wake: the water under the pass is lifted, and falls back. */
        strength = 1 + (int)(speed / 200.0f);
        for (offset = -FLUID_FLYOVER_HALF_WIDTH; offset <= FLUID_FLYOVER_HALF_WIDTH;
             ++offset) {
            if (WorldPushLiquid(world, x + offset, probe, 0, -1, strength)) {
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
            .radius = (float)(2 * FLUID_FLYOVER_HALF_WIDTH + 1),
            .material = material,
        });
        return;
    }
}

void FluidInteractionUpdatePlayer(FluidInteractionState *state, Player *player,
                                  World *world, GameEventBuffer *events,
                                  float deltaTime)
{
    float speed;
    float fraction;
    CellMaterial liquid;

    if (state == NULL || player == NULL || world == NULL || world->cells == NULL ||
        events == NULL) {
        return;
    }
    speed = sqrtf(player->velocity.x * player->velocity.x +
                  player->velocity.y * player->velocity.y);
    fraction = FluidSubmergedFraction(world, player, &liquid);
    state->submerged = fraction;
    if (fraction > 0.0f) {
        state->liquid = liquid;
    }
    state->flyoverCooldown -= deltaTime;

    /* The drag the character integrates this frame: nothing in air, the full
       coefficient under water, and in proportion between. */
    player->fluidDrag = state->config.dragCoefficient * fraction;
    if (fraction > 0.0f && state->liquid == MATERIAL_LAVA) {
        player->fluidDrag *= state->config.lavaDragScale;
    }

    if (!state->inside && fraction >= FLUID_ENTER_FRACTION) {
        state->inside = true;
        ++state->stats.entries;
        if (speed >= state->config.splashSpeed) {
            /* Hitting the surface costs a share of the speed outright — the
               blow of it — before the drag takes the rest. */
            player->velocity.x *= 1.0f - state->config.entryLoss;
            player->velocity.y *= 1.0f - state->config.entryLoss;
            FluidSplash(state, player, world, events, speed, true);
        }
    } else if (state->inside && fraction <= FLUID_LEAVE_FRACTION) {
        state->inside = false;
        ++state->stats.exits;
        if (speed >= state->config.splashSpeed) {
            FluidSplash(state, player, world, events, speed, false);
        }
    } else if (!state->inside && fraction <= 0.0f &&
               speed >= state->config.flyoverSpeed &&
               state->flyoverCooldown <= 0.0f) {
        FluidFlyover(state, player, world, events, speed);
    }
}
