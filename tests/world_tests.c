/* Headless invariant tests for the simulation core.
 *
 * These never open a window and never touch the GPU: WorldInit allocates only
 * CPU state, and WorldInitRenderer (the one function that needs a GL context)
 * is deliberately not called here. That keeps the suite fast enough to run on
 * every build and lets it cover many more cases than the windowed smoke test.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "particles.h"
#include "player.h"
#include "powers.h"
#include "world.h"

static int testsRun = 0;
static int testsFailed = 0;
static const char *currentTest = "";

#define CHECK(condition, ...)                                                  \
    do {                                                                       \
        if (!(condition)) {                                                    \
            ++testsFailed;                                                     \
            fprintf(stderr, "FAIL %s: %s\n      ", currentTest, #condition);   \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            return;                                                            \
        }                                                                      \
    } while (0)

#define RUN(test)                                                              \
    do {                                                                       \
        int failedBefore = testsFailed;                                        \
        currentTest = #test;                                                   \
        ++testsRun;                                                            \
        test();                                                                \
        if (testsFailed == failedBefore) {                                     \
            printf("ok   %s\n", #test);                                        \
        }                                                                      \
    } while (0)

static int CountMaterial(const World *world, CellMaterial material)
{
    int count = 0;
    int y;

    for (y = 0; y < world->height; ++y) {
        int x;

        for (x = 0; x < world->width; ++x) {
            if (WorldGetCell(world, x, y) == material) {
                ++count;
            }
        }
    }
    return count;
}

static void FillRect(World *world, int x0, int y0, int x1, int y1,
                     CellMaterial material)
{
    int y;

    for (y = y0; y <= y1; ++y) {
        int x;

        for (x = x0; x <= x1; ++x) {
            WorldSetCell(world, x, y, material);
        }
    }
}

static void Tick(World *world, int count)
{
    int i;

    for (i = 0; i < count; ++i) {
        WorldUpdate(world);
    }
}

/* --- material table ----------------------------------------------------- */

static void test_every_material_has_table_data(void)
{
    int material;

    for (material = 0; material < MATERIAL_COUNT; ++material) {
        const char *name = WorldMaterialName((CellMaterial)material);

        CHECK(name != NULL, "material %d has no name", material);
        CHECK(name[0] != '\0', "material %d has an empty name", material);
        /* A gap in the table is zero-filled, which reads back as the EMPTY
           entry: that is exactly the silent failure the table has to catch. */
        CHECK(material == MATERIAL_EMPTY || strcmp(name, "EMPTY") != 0,
              "material %d fell back to the EMPTY entry", material);
    }
}

static void test_solidity_matches_what_blocks_the_player(void)
{
    CHECK(WorldMaterialIsSolid(MATERIAL_DIRT), "dirt must be solid");
    CHECK(WorldMaterialIsSolid(MATERIAL_ROCK), "rock must be solid");
    CHECK(WorldMaterialIsSolid(MATERIAL_SAND), "sand must be solid");
    CHECK(!WorldMaterialIsSolid(MATERIAL_EMPTY), "empty must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_WATER), "water must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_LAVA), "lava must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_STEAM), "steam must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_SMOKE), "smoke must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_FIRE), "fire must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_ASH), "ash must be passable");
}

/* --- movement ---------------------------------------------------------- */

static void test_sand_falls_to_the_floor_and_is_conserved(void)
{
    World world;
    int before;

    CHECK(WorldInit(&world, 64, 48), "world allocation failed");
    FillRect(&world, 30, 4, 33, 11, MATERIAL_SAND);
    before = CountMaterial(&world, MATERIAL_SAND);
    Tick(&world, 300);

    CHECK(CountMaterial(&world, MATERIAL_SAND) == before,
          "sand count changed from %d to %d", before,
          CountMaterial(&world, MATERIAL_SAND));
    CHECK(CountMaterial(&world, MATERIAL_SAND) == 32, "expected 32 sand cells");
    CHECK(WorldGetCell(&world, 31, 47) == MATERIAL_SAND,
          "sand did not reach the floor");
    CHECK(WorldGetCell(&world, 31, 4) == MATERIAL_EMPTY,
          "sand did not leave its starting cell");
    WorldUnload(&world);
}

static void test_sand_falls_at_most_one_cell_per_tick(void)
{
    World world;
    int tick;

    CHECK(WorldInit(&world, 32, 64), "world allocation failed");
    WorldSetCell(&world, 16, 2, MATERIAL_SAND);

    /* A cell that has already moved this tick must not be simulated again, so
       a free-falling grain advances exactly one row per tick. */
    for (tick = 1; tick <= 20; ++tick) {
        WorldUpdate(&world);
        CHECK(WorldGetCell(&world, 16, 2 + tick) == MATERIAL_SAND,
              "after %d ticks the grain was not at row %d", tick, 2 + tick);
    }
    WorldUnload(&world);
}

static void test_water_spreads_sideways_and_is_conserved(void)
{
    World world;
    int before;
    int minimumX = 1 << 30;
    int maximumX = -1;
    int highest = 1 << 30;
    int x;
    int y;

    CHECK(WorldInit(&world, 64, 48), "world allocation failed");
    FillRect(&world, 0, 47, 63, 47, MATERIAL_ROCK);
    FillRect(&world, 30, 30, 33, 40, MATERIAL_WATER);
    before = CountMaterial(&world, MATERIAL_WATER);
    Tick(&world, 400);

    CHECK(CountMaterial(&world, MATERIAL_WATER) == before,
          "water count changed from %d to %d", before,
          CountMaterial(&world, MATERIAL_WATER));

    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) != MATERIAL_WATER) {
                continue;
            }
            if (x < minimumX) minimumX = x;
            if (x > maximumX) maximumX = x;
            if (y < highest) highest = y;
        }
    }

    /* The column was four cells wide and eleven tall; once it lands it must
       slump into a much wider, much shallower mound. */
    CHECK(maximumX - minimumX >= 20,
          "water spanned only x=[%d..%d]", minimumX, maximumX);
    CHECK(highest >= 40, "water piled up to row %d instead of slumping", highest);
    WorldUnload(&world);
}

/* --- thermal ----------------------------------------------------------- */

static void test_rock_becomes_lava_above_its_threshold(void)
{
    World world;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    WorldSetCell(&world, 16, 16, MATERIAL_ROCK);
    WorldSetTemperature(&world, 16, 16, 760.0f);
    WorldUpdate(&world);

    CHECK(WorldGetCell(&world, 16, 16) == MATERIAL_LAVA,
          "rock above 720C did not melt");
    WorldUnload(&world);
}

static void test_water_becomes_steam_above_its_threshold(void)
{
    World world;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    WorldSetCell(&world, 16, 16, MATERIAL_WATER);
    WorldSetTemperature(&world, 16, 16, 120.0f);
    WorldUpdate(&world);

    CHECK(WorldGetCell(&world, 16, 16) == MATERIAL_STEAM,
          "water above 108C did not boil");
    WorldUnload(&world);
}

static void test_water_and_lava_react_into_steam_and_rock(void)
{
    World world;
    bool reactionSeen = false;
    int tick;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    FillRect(&world, 0, 31, 31, 31, MATERIAL_ROCK);
    WorldSetCell(&world, 16, 20, MATERIAL_WATER);
    WorldSetCell(&world, 17, 20, MATERIAL_LAVA);

    for (tick = 0; tick < 60 && !reactionSeen; ++tick) {
        WorldUpdate(&world);
        reactionSeen = world.reactionCount > 0;
    }

    CHECK(reactionSeen, "no reaction event was published");
    CHECK(CountMaterial(&world, MATERIAL_STEAM) > 0, "no steam was produced");
    WorldUnload(&world);
}

static void test_one_fire_cell_cannot_consume_a_whole_dirt_field(void)
{
    /* The containment budget is the invariant that keeps fire from eating an
       unlimited connected dirt layer. Check it from several ignition points. */
    static const int ignitionX[] = {24, 10, 39, 24};
    static const int ignitionY[] = {16, 9, 23, 9};
    int probe;

    for (probe = 0; probe < 4; ++probe) {
        World world;
        int dirtBefore;
        int dirtAfter;

        CHECK(WorldInit(&world, 48, 32), "world allocation failed");
        FillRect(&world, 8, 8, 40, 24, MATERIAL_DIRT);
        dirtBefore = CountMaterial(&world, MATERIAL_DIRT);
        WorldSetCell(&world, ignitionX[probe], ignitionY[probe], MATERIAL_FIRE);
        Tick(&world, 400);
        dirtAfter = CountMaterial(&world, MATERIAL_DIRT);

        CHECK(dirtAfter > dirtBefore / 2,
              "ignition %d burned %d of %d dirt cells", probe,
              dirtBefore - dirtAfter, dirtBefore);
        CHECK(CountMaterial(&world, MATERIAL_FIRE) == 0,
              "ignition %d still burning after 400 ticks", probe);
        WorldUnload(&world);
    }
}

static void test_a_lava_pocket_cannot_consume_its_rock_lining(void)
{
    World world;
    int lavaBefore;
    int rockBefore;

    CHECK(WorldInit(&world, 128, 128), "world allocation failed");
    FillRect(&world, 0, 0, 127, 127, MATERIAL_ROCK);
    FillRect(&world, 40, 50, 79, 69, MATERIAL_LAVA);
    lavaBefore = CountMaterial(&world, MATERIAL_LAVA);
    rockBefore = CountMaterial(&world, MATERIAL_ROCK);

    /* Lava heats what it touches, but rock must asymptote below its melt point.
       Without that budget a single pocket turns the whole map to lava, exactly
       as an unbudgeted fire would burn every connected dirt cell. */
    Tick(&world, 3000);

    CHECK(CountMaterial(&world, MATERIAL_LAVA) == lavaBefore,
          "lava grew from %d to %d cells", lavaBefore,
          CountMaterial(&world, MATERIAL_LAVA));
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == rockBefore,
          "lava melted %d rock cells",
          rockBefore - CountMaterial(&world, MATERIAL_ROCK));
    WorldUnload(&world);
}

static void test_lava_still_ignites_dirt_it_touches(void)
{
    World world;
    bool burned = false;
    int tick;

    /* The melt budget must not make lava thermally inert: dirt ignites far
       below rock's threshold and still has to catch. */
    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_DIRT);
    FillRect(&world, 28, 28, 35, 35, MATERIAL_LAVA);

    for (tick = 0; tick < 600 && !burned; ++tick) {
        WorldUpdate(&world);
        burned = CountMaterial(&world, MATERIAL_FIRE) > 0 ||
                 CountMaterial(&world, MATERIAL_DIRT) < 64 * 64 - 64;
    }
    CHECK(burned, "lava never ignited the dirt around it");
    WorldUnload(&world);
}

static void test_settled_cells_sleep_but_wake_when_disturbed(void)
{
    World world;
    int settledChunks;

    CHECK(WorldInit(&world, 128, 128), "world allocation failed");
    FillRect(&world, 0, 100, 127, 127, MATERIAL_ROCK);
    FillRect(&world, 40, 90, 60, 99, MATERIAL_SAND);
    Tick(&world, 400);
    settledChunks = world.activeChunkCount;

    /* A pile that cannot move is not work: it must let its chunks sleep. */
    CHECK(settledChunks == 0, "settled sand kept %d chunks awake", settledChunks);

    /* ...but pulling the floor out from under it has to wake it again. */
    FillRect(&world, 40, 100, 60, 127, MATERIAL_EMPTY);
    WorldUpdate(&world);
    CHECK(world.activeChunkCount > 0, "removing the floor woke nothing");
    Tick(&world, 200);
    CHECK(WorldGetCell(&world, 50, 127) == MATERIAL_SAND,
          "sand did not fall into the space that opened below it");
    WorldUnload(&world);
}

/* --- drilling ---------------------------------------------------------- */

static void test_drill_removes_solids_and_leaves_liquids(void)
{
    World world;
    int waterBefore;
    int lavaBefore;

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_ROCK);
    FillRect(&world, 28, 28, 35, 35, MATERIAL_WATER);
    FillRect(&world, 28, 36, 35, 40, MATERIAL_LAVA);
    waterBefore = CountMaterial(&world, MATERIAL_WATER);
    lavaBefore = CountMaterial(&world, MATERIAL_LAVA);

    CHECK(WorldDrillCircle(&world, 32, 32, 6) > 0, "drill removed nothing");
    CHECK(CountMaterial(&world, MATERIAL_WATER) == waterBefore,
          "drill destroyed %d water cells",
          waterBefore - CountMaterial(&world, MATERIAL_WATER));
    CHECK(CountMaterial(&world, MATERIAL_LAVA) == lavaBefore,
          "drill destroyed %d lava cells",
          lavaBefore - CountMaterial(&world, MATERIAL_LAVA));
    WorldUnload(&world);
}

static void test_drill_returns_the_number_of_cells_it_removed(void)
{
    World world;
    int solidBefore;
    int removed;

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_ROCK);
    solidBefore = CountMaterial(&world, MATERIAL_ROCK);
    removed = WorldDrillCircle(&world, 32, 32, 5);

    /* A fraction of the cut becomes ash instead of empty, so the removed count
       must match rock lost, not the empty cells created. */
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == solidBefore - removed,
          "removed %d but rock fell by %d", removed,
          solidBefore - CountMaterial(&world, MATERIAL_ROCK));
    CHECK(WorldDrillCircle(&world, 32, 32, 5) == 0,
          "drilling the same hole twice removed cells again");
    WorldUnload(&world);
}

static void test_drill_cannot_breach_the_world_boundary(void)
{
    World world;
    int x;
    int y;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    FillRect(&world, 0, 0, 31, 31, MATERIAL_ROCK);
    /* Centred outside the grid, and straddling every edge. */
    WorldDrillCircle(&world, -8, 16, 6);
    WorldDrillCircle(&world, 40, 16, 6);
    WorldDrillCircle(&world, 16, -8, 6);
    WorldDrillCircle(&world, 16, 40, 6);
    WorldDrillCircle(&world, 0, 0, 4);

    for (y = 0; y < 32; ++y) {
        for (x = 0; x < 32; ++x) {
            CHECK(WorldGetCell(&world, x, y) != MATERIAL_EMPTY ||
                      (x < 6 && y < 6),
                  "cell %d,%d was cleared from outside the grid", x, y);
        }
    }
    WorldUnload(&world);
}

static void test_drill_heat_cannot_ignite_dirt_or_boil_water(void)
{
    World world;
    int pass;

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_DIRT);
    FillRect(&world, 40, 20, 47, 44, MATERIAL_WATER);

    /* Repeated passes must not accumulate heat past the phase thresholds. */
    for (pass = 0; pass < 40; ++pass) {
        WorldDrillCircle(&world, 20 + pass % 8, 32, 5);
        WorldUpdate(&world);
    }
    Tick(&world, 60);

    CHECK(CountMaterial(&world, MATERIAL_FIRE) == 0,
          "drilling ignited %d dirt cells", CountMaterial(&world, MATERIAL_FIRE));
    CHECK(CountMaterial(&world, MATERIAL_STEAM) == 0,
          "drilling boiled %d water cells", CountMaterial(&world, MATERIAL_STEAM));
    WorldUnload(&world);
}

/* --- chunk scheduling -------------------------------------------------- */

static void test_a_settled_world_lets_chunks_sleep(void)
{
    World world;
    int totalChunks;

    CHECK(WorldInit(&world, 256, 256), "world allocation failed");
    FillRect(&world, 0, 200, 255, 255, MATERIAL_ROCK);
    Tick(&world, 120);
    totalChunks = world.chunkColumns * world.chunkRows;

    CHECK(world.activeChunkCount < totalChunks,
          "all %d chunks stayed awake in a static world", totalChunks);
    CHECK(WorldCountDynamicCells(&world) == 0,
          "static rock counted as %d dynamic cells",
          WorldCountDynamicCells(&world));
    WorldUnload(&world);
}

static void test_activity_wakes_only_a_local_neighbourhood(void)
{
    World world;

    CHECK(WorldInit(&world, 256, 256), "world allocation failed");
    Tick(&world, 8);
    CHECK(world.activeChunkCount == 0, "empty world kept %d chunks awake",
          world.activeChunkCount);

    WorldSetCell(&world, 128, 128, MATERIAL_SAND);
    WorldUpdate(&world);
    CHECK(world.activeChunkCount > 0, "a falling grain woke no chunk");
    CHECK(world.activeChunkCount <= 9,
          "one grain woke %d chunks", world.activeChunkCount);
    WorldUnload(&world);
}

/* --- player against the world ------------------------------------------ */

static void test_boosting_player_tunnels_through_rock(void)
{
    World world;
    Player player;
    int step;
    int drilled = 0;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 60, 40, 200, 90, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){20.0f, 64.0f});

    for (step = 0; step < 120; ++step) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true, 1.0f / 60.0f);
        drilled += player.drilledCells;
    }

    CHECK(drilled > 0, "the player never cut a cell");
    CHECK(player.position.x > 200.0f,
          "the player stalled at x=%.1f inside the rock", player.position.x);
    WorldUnload(&world);
}

static void test_boost_from_rest_bores_into_a_wall(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 128, 200), "world allocation failed");
    /* The player starts resting on solid ground, the way a spawn does, and
       boosts straight down into it. */
    FillRect(&world, 0, 40, 127, 199, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){64.0f, 32.0f});

    for (step = 0; step < 240; ++step) {
        PlayerUpdate(&player, &world, (Vector2){0.0f, 1.0f}, true, 1.0f / 60.0f);
        WorldUpdate(&world);
        PlayerResolveWorldCollision(&player, &world);
    }

    /* Below the drill threshold the collision zeroes the blocked velocity every
       frame, so without a contact drill the speed can never climb to the
       threshold and the boost stalls on the surface forever. */
    CHECK(player.position.y > 120.0f,
          "the player never broke the surface, stopping at y=%.1f",
          player.position.y);
    WorldUnload(&world);
}

static void test_boosting_player_tunnels_through_sand(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 60, 40, 200, 90, MATERIAL_SAND);
    PlayerInit(&player, (Vector2){20.0f, 64.0f});

    /* Sand is dynamic, so this has to run a whole frame the way main.c does:
       the drill cuts, the world settles into the fresh tunnel, and only then is
       embedding resolved. Driving PlayerUpdate alone would never see it. */
    for (step = 0; step < 240; ++step) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true, 1.0f / 60.0f);
        WorldUpdate(&world);
        PlayerResolveWorldCollision(&player, &world);
    }

    CHECK(player.position.x > 200.0f,
          "the player stalled at x=%.1f inside the sand", player.position.x);
    WorldUnload(&world);
}

static int CountActiveParticles(const ParticleSystem *particles)
{
    int i;
    int count = 0;

    for (i = 0; i < MAX_PARTICLES; ++i) {
        if (particles->particles[i].active) {
            ++count;
        }
    }
    return count;
}

static void test_cryo_does_not_destroy_solid_terrain(void)
{
    World world;
    int step;
    int solidBefore;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 0, 30, 95, 40, MATERIAL_ROCK);
    FillRect(&world, 0, 41, 95, 48, MATERIAL_DIRT);
    FillRect(&world, 0, 49, 95, 56, MATERIAL_SAND);
    solidBefore = CountMaterial(&world, MATERIAL_ROCK) +
                  CountMaterial(&world, MATERIAL_DIRT) +
                  CountMaterial(&world, MATERIAL_SAND);

    /* Cold is not a solvent. Rock, dirt and sand have no cooling transition, so
       chilling them far below ambient must do nothing but make them cold.
       Encoding "no transition" as a zero-filled field once made every one of
       them evaporate at 0C, and the cryo beam bored through the map. */
    for (step = 0; step < 240; ++step) {
        WorldApplyChill(&world, (Vector2){48.0f, 5.0f}, (Vector2){48.0f, 60.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }

    CHECK(CountMaterial(&world, MATERIAL_ROCK) +
                  CountMaterial(&world, MATERIAL_DIRT) +
                  CountMaterial(&world, MATERIAL_SAND) ==
              solidBefore,
          "the cryo beam destroyed terrain: %d solid cells became %d", solidBefore,
          CountMaterial(&world, MATERIAL_ROCK) + CountMaterial(&world, MATERIAL_DIRT) +
              CountMaterial(&world, MATERIAL_SAND));
    CHECK(WorldGetTemperature(&world, 48, 31) < 0.0f,
          "the beam did not even chill the rock face it stopped against (%.1f C)",
          WorldGetTemperature(&world, 48, 31));
    WorldUnload(&world);
}

static int CountFilled(const World *world)
{
    int x;
    int y;
    int filled = 0;

    for (y = 0; y < world->height; ++y) {
        for (x = 0; x < world->width; ++x) {
            if (WorldGetCell(world, x, y) != MATERIAL_EMPTY) {
                ++filled;
            }
        }
    }
    return filled;
}

static void test_no_material_evaporates_when_merely_cooled(void)
{
    CellMaterial material;

    /* The whole-table version of the test above, and the one that catches the
       next material added without a deliberate cooling entry. Chilling may turn
       a material into a different one — water into ice, lava into rock, fire
       into smoke — but it must never make matter disappear. A few ticks only,
       so the lifetimes of fire, smoke and steam cannot confuse the count. */
    for (material = 0; material < MATERIAL_COUNT; ++material) {
        World world;
        int x;
        int y;
        int before;

        if (material == MATERIAL_EMPTY) {
            continue;
        }
        CHECK(WorldInit(&world, 32, 40), "world allocation failed");
        for (y = 10; y < 20; ++y) {
            for (x = 10; x < 20; ++x) {
                WorldSetCell(&world, x, y, material);
                WorldSetTemperature(&world, x, y, -260.0f);
            }
        }
        before = CountFilled(&world);
        Tick(&world, 4);
        CHECK(CountFilled(&world) == before,
              "%s lost cells when chilled: %d filled became %d",
              WorldMaterialName(material), before, CountFilled(&world));
        WorldUnload(&world);
    }
}

static void test_cryo_snuffs_fire_into_smoke(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 64, 48), "world allocation failed");
    FillRect(&world, 24, 24, 40, 30, MATERIAL_FIRE);

    for (step = 0; step < 60; ++step) {
        WorldApplyChill(&world, (Vector2){32.0f, 5.0f}, (Vector2){32.0f, 44.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    CHECK(CountMaterial(&world, MATERIAL_SMOKE) > 0,
          "chilled fire left no smoke behind");
    WorldUnload(&world);
}

static void test_cryo_freezes_water_into_standing_ice(void)
{
    World world;
    int step;
    int ice;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 20, 40, 76, 50, MATERIAL_WATER);

    for (step = 0; step < 60; ++step) {
        WorldApplyChill(&world, (Vector2){48.0f, 20.0f}, (Vector2){48.0f, 60.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    ice = CountMaterial(&world, MATERIAL_ICE);
    CHECK(ice > 0, "the cryo beam froze nothing");
    CHECK(WorldMaterialIsSolid(MATERIAL_ICE), "ice must be solid to build with");

    /* Ice does not drift back on its own, so it is something the player can
       actually build with. */
    Tick(&world, 600);
    CHECK(CountMaterial(&world, MATERIAL_ICE) == ice,
          "undisturbed ice changed on its own: %d cells became %d", ice,
          CountMaterial(&world, MATERIAL_ICE));
    WorldUnload(&world);
}

static void test_heat_melts_ice_back_into_water(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 30, 40, 60, 46, MATERIAL_ICE);

    /* Stable is not permanent: anything warm has to undo it, or the player
       could seal the world shut with something nothing can remove. */
    for (step = 0; step < 60; ++step) {
        WorldApplyLaser(&world, (Vector2){45.0f, 20.0f}, (Vector2){45.0f, 60.0f},
                        2.25f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    CHECK(CountMaterial(&world, MATERIAL_WATER) > 0,
          "the laser never melted any ice");
    WorldUnload(&world);
}

static void test_cryo_settles_lava_back_into_rock(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 30, 40, 66, 50, MATERIAL_LAVA);

    /* Lava relaxes back toward 900C every tick, so this only passes if the beam
       out-cools that relaxation rather than merely dipping the temperature. */
    for (step = 0; step < 120; ++step) {
        WorldApplyChill(&world, (Vector2){48.0f, 20.0f}, (Vector2){48.0f, 60.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    CHECK(CountMaterial(&world, MATERIAL_ROCK) > 0,
          "sustained cryo never turned any lava into rock");
    WorldUnload(&world);
}

static void test_one_force_blast_throws_loose_material_far(void)
{
    World world;
    int before;
    int rightmost = 0;
    int x;
    int y;

    CHECK(WorldInit(&world, 160, 64), "world allocation failed");
    FillRect(&world, 30, 30, 40, 40, MATERIAL_SAND);
    before = CountMaterial(&world, MATERIAL_SAND);

    /* A single blow, not a stream: the whole point is that one press moves the
       world a long way. */
    WorldApplyForceBlast(&world, (Vector2){20.0f, 35.0f}, (Vector2){1.0f, 0.0f},
                         62.0f, 0.82f, 34);

    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_SAND && x > rightmost) {
                rightmost = x;
            }
        }
    }

    CHECK(CountMaterial(&world, MATERIAL_SAND) == before,
          "the blast destroyed loose material: %d sand cells became %d", before,
          CountMaterial(&world, MATERIAL_SAND));
    CHECK(rightmost > 55, "one blast barely moved anything; rightmost sand at x=%d",
          rightmost);
    WorldUnload(&world);
}

static void test_configured_force_power_hits_far_and_hard(void)
{
    World world;
    PowerSystem powers;
    ParticleSystem particles;
    int rightmost = 0;
    int activeParticles = 0;
    int before;
    int x;
    int y;
    int i;

    CHECK(WorldInit(&world, 192, 72), "world allocation failed");
    FillRect(&world, 30, 30, 40, 40, MATERIAL_SAND);
    before = CountMaterial(&world, MATERIAL_SAND);
    PowersInit(&powers);
    ParticlesInit(&particles);

    PowersUpdate(&powers, &world, &particles, (Vector2){20.0f, 35.0f},
                 (Vector2){150.0f, 35.0f}, 1.0f / 60.0f, false, false, true,
                 false);

    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_SAND && x > rightmost) {
                rightmost = x;
            }
        }
    }
    for (i = 0; i < MAX_PARTICLES; ++i) {
        if (particles.particles[i].active) {
            ++activeParticles;
        }
    }

    CHECK(powers.forceTriggered, "Q press did not publish a force event");
    CHECK(CountMaterial(&world, MATERIAL_SAND) == before,
          "configured force destroyed loose cells");
    CHECK(rightmost > 76,
          "configured Q still reads as a weak shove; rightmost sand at x=%d",
          rightmost);
    CHECK(activeParticles >= 48,
          "configured Q spawned only %d burst particles", activeParticles);
    CHECK(powers.forceRecoil >= 120.0f,
          "configured recoil is too small for a heavy hit: %.1f",
          powers.forceRecoil);
    WorldUnload(&world);
}

static void test_force_blast_marks_solid_terrain_without_boring_through_it(void)
{
    World world;
    int before;
    int step;
    int remaining;
    int interior = 0;
    int x;
    int y;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 30, 20, 60, 50, MATERIAL_ROCK);
    before = CountMaterial(&world, MATERIAL_ROCK);

    for (step = 0; step < 20; ++step) {
        WorldApplyForceBlast(&world, (Vector2){20.0f, 35.0f}, (Vector2){1.0f, 0.0f},
                             62.0f, 0.82f, 34);
        WorldUpdate(&world);
    }

    remaining = CountMaterial(&world, MATERIAL_ROCK);
    CHECK(remaining < before, "the blast left no mark on the rock face at all");
    /* A dent, not a tunnel: the wall must still be a wall after twenty blows. */
    CHECK(remaining > before * 4 / 5,
          "the blast bored through solid terrain: %d rock cells became %d", before,
          remaining);

    /* Only the exposed face is scoured; the inside of the block is untouched. */
    for (y = 25; y <= 45; ++y) {
        for (x = 40; x <= 58; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_ROCK) {
                ++interior;
            }
        }
    }
    CHECK(interior == 21 * 19, "the blast hollowed out rock behind the surface");
    WorldUnload(&world);
}

static void test_force_blast_does_not_reach_behind_a_wall(void)
{
    World world;
    int step;
    int x;
    int y;
    int moved = 0;

    CHECK(WorldInit(&world, 160, 64), "world allocation failed");
    /* A wall across the cone, with loose sand sheltering behind it. */
    FillRect(&world, 50, 0, 54, 63, MATERIAL_ROCK);
    FillRect(&world, 70, 30, 80, 40, MATERIAL_SAND);

    for (step = 0; step < 10; ++step) {
        WorldApplyForceBlast(&world, (Vector2){20.0f, 35.0f}, (Vector2){1.0f, 0.0f},
                             62.0f, 0.82f, 34);
    }

    /* No settling ticks: any sand outside its original block can only have been
       thrown, and the wall should have absorbed the whole blow. */
    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_SAND &&
                (x < 70 || x > 80 || y < 30 || y > 40)) {
                ++moved;
            }
        }
    }
    CHECK(moved == 0, "%d sheltered cells were thrown through solid rock", moved);
    WorldUnload(&world);
}

static void test_bouncing_particles_do_not_pass_through_terrain(void)
{
    World world;
    ParticleSystem particles;
    int step;
    int inside = 0;
    int i;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 70, 0, 127, 63, MATERIAL_ROCK);
    ParticlesInit(&particles);

    /* Impact sparks fired straight at a rock wall from open air. */
    ParticlesSpawnImpact(&particles, (Vector2){50.0f, 32.0f},
                         (Vector2){1.0f, 0.0f}, 90.0f);
    CHECK(CountActiveParticles(&particles) > 0, "no particles were spawned");

    for (step = 0; step < 60; ++step) {
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
        for (i = 0; i < MAX_PARTICLES; ++i) {
            const Particle *particle = &particles.particles[i];

            if (particle->active && particle->position.x >= 71.0f) {
                ++inside;
            }
        }
    }

    CHECK(inside == 0, "%d particle samples ended up inside the rock", inside);
    WorldUnload(&world);
}

static void test_drill_debris_settles_as_ash_without_overwriting_terrain(void)
{
    World world;
    ParticleSystem particles;
    int step;
    int rockBefore;
    int rockAfter;
    int ash;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 0, 40, 127, 63, MATERIAL_ROCK);
    rockBefore = CountMaterial(&world, MATERIAL_ROCK);
    ParticlesInit(&particles);

    for (step = 0; step < 40; ++step) {
        ParticlesSpawnDrillDebris(&particles, (Vector2){64.0f, 30.0f},
                                  (Vector2){0.0f, -120.0f}, 30);
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
    }
    for (step = 0; step < 120; ++step) {
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
    }

    ash = CountMaterial(&world, MATERIAL_ASH);
    rockAfter = CountMaterial(&world, MATERIAL_ROCK);
    CHECK(ash > 0, "debris never settled into the world");
    CHECK(rockAfter == rockBefore,
          "settling debris overwrote terrain: %d rock cells became %d",
          rockBefore, rockAfter);
    WorldUnload(&world);
}

static void test_passing_particles_ignore_terrain(void)
{
    World world;
    ParticleSystem particles;
    int step;
    int i;
    bool crossed = false;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 0, 20, 127, 24, MATERIAL_ROCK);
    ParticlesInit(&particles);

    /* Steam is a gas effect: a rock ceiling must not stop it, or reaction plumes
       would pile up against the lid of a pocket instead of drifting. */
    ParticlesSpawnSteam(&particles, (Vector2){64.0f, 40.0f});
    for (step = 0; step < 120 && !crossed; ++step) {
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
        for (i = 0; i < MAX_PARTICLES; ++i) {
            const Particle *particle = &particles.particles[i];

            if (particle->active && particle->position.y < 20.0f) {
                crossed = true;
            }
        }
    }

    CHECK(crossed, "steam never rose past the rock ceiling");
    WorldUnload(&world);
}

static void test_drill_resistance_never_stalls_the_boost(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 40, 20, 255, 110, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){20.0f, 64.0f});

    for (step = 0; step < 200; ++step) {
        float speed;

        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true, 1.0f / 60.0f);
        speed = sqrtf(player.velocity.x * player.velocity.x +
                      player.velocity.y * player.velocity.y);
        /* Once inside solid terrain the drill must keep the player above its
           own threshold, otherwise a boost would bury itself. */
        if (player.position.x > 60.0f && player.position.x < 240.0f) {
            CHECK(speed >= player.drillSpeed,
                  "speed fell to %.1f at x=%.1f, below the %.1f drill threshold",
                  speed, player.position.x, player.drillSpeed);
        }
    }
    WorldUnload(&world);
}

static void test_normal_flight_keeps_a_hover_pose(void)
{
    World world;
    Player player;
    int frame;

    CHECK(WorldInit(&world, 512, 128), "world allocation failed");
    PlayerInit(&player, (Vector2){48.0f, 64.0f});

    for (frame = 0; frame < 180; ++frame) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, false,
                     1.0f / 120.0f);
    }

    CHECK(!player.boosting, "normal-flight test unexpectedly enabled boost");
    CHECK(player.velocity.x > player.maxSpeed * 0.95f,
          "normal flight never reached cruise speed: %.1f", player.velocity.x);
    CHECK(fabsf(player.leanAmount - 0.12f) < 0.02f,
          "normal flight stopped reading as a slight hover lean: %.3f",
          player.leanAmount);
    WorldUnload(&world);
}

static void test_boost_flight_reaches_a_head_first_pose(void)
{
    World world;
    Player player;
    int frame;

    CHECK(WorldInit(&world, 512, 128), "world allocation failed");
    PlayerInit(&player, (Vector2){48.0f, 64.0f});

    for (frame = 0; frame < 180; ++frame) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true,
                     1.0f / 120.0f);
    }

    CHECK(player.boosting, "boost-pose test never enabled boost");
    CHECK(player.leanAmount > 0.9f,
          "boost never reached its head-first pose: %.3f", player.leanAmount);
    WorldUnload(&world);
}

static void test_player_never_ends_a_frame_inside_solid_terrain(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 0, 70, 255, 127, MATERIAL_ROCK);
    FillRect(&world, 90, 20, 130, 70, MATERIAL_DIRT);
    PlayerInit(&player, (Vector2){20.0f, 40.0f});

    for (step = 0; step < 400; ++step) {
        /* Sweep the input around so the player grinds along walls and corners
           rather than following one clean line. */
        float angle = (float)step * 0.11f;
        Vector2 input = {cosf(angle), sinf(angle)};
        int radius;
        int checkedX;
        int checkedY;
        bool embedded = false;

        PlayerUpdate(&player, &world, input, (step % 3) != 0, 1.0f / 60.0f);
        WorldUpdate(&world);
        PlayerResolveWorldCollision(&player, &world);

        radius = (int)player.radius;
        for (checkedY = -radius; checkedY <= radius && !embedded; ++checkedY) {
            for (checkedX = -radius; checkedX <= radius; ++checkedX) {
                int cellX = (int)player.position.x + checkedX;
                int cellY = (int)player.position.y + checkedY;

                if (checkedX * checkedX + checkedY * checkedY > radius * radius) {
                    continue;
                }
                if (WorldMaterialIsSolid(WorldGetCell(&world, cellX, cellY))) {
                    embedded = true;
                    break;
                }
            }
        }
        CHECK(!embedded, "player embedded in solid terrain at %.1f,%.1f on step %d",
              player.position.x, player.position.y, step);
    }
    WorldUnload(&world);
}

int main(void)
{
    RUN(test_every_material_has_table_data);
    RUN(test_solidity_matches_what_blocks_the_player);
    RUN(test_sand_falls_to_the_floor_and_is_conserved);
    RUN(test_sand_falls_at_most_one_cell_per_tick);
    RUN(test_water_spreads_sideways_and_is_conserved);
    RUN(test_rock_becomes_lava_above_its_threshold);
    RUN(test_water_becomes_steam_above_its_threshold);
    RUN(test_water_and_lava_react_into_steam_and_rock);
    RUN(test_one_fire_cell_cannot_consume_a_whole_dirt_field);
    RUN(test_a_lava_pocket_cannot_consume_its_rock_lining);
    RUN(test_lava_still_ignites_dirt_it_touches);
    RUN(test_settled_cells_sleep_but_wake_when_disturbed);
    RUN(test_drill_removes_solids_and_leaves_liquids);
    RUN(test_drill_returns_the_number_of_cells_it_removed);
    RUN(test_drill_cannot_breach_the_world_boundary);
    RUN(test_drill_heat_cannot_ignite_dirt_or_boil_water);
    RUN(test_a_settled_world_lets_chunks_sleep);
    RUN(test_activity_wakes_only_a_local_neighbourhood);
    RUN(test_boosting_player_tunnels_through_rock);
    RUN(test_boost_from_rest_bores_into_a_wall);
    RUN(test_boosting_player_tunnels_through_sand);
    RUN(test_drill_resistance_never_stalls_the_boost);
    RUN(test_normal_flight_keeps_a_hover_pose);
    RUN(test_boost_flight_reaches_a_head_first_pose);
    RUN(test_cryo_does_not_destroy_solid_terrain);
    RUN(test_no_material_evaporates_when_merely_cooled);
    RUN(test_cryo_snuffs_fire_into_smoke);
    RUN(test_cryo_freezes_water_into_standing_ice);
    RUN(test_heat_melts_ice_back_into_water);
    RUN(test_cryo_settles_lava_back_into_rock);
    RUN(test_one_force_blast_throws_loose_material_far);
    RUN(test_configured_force_power_hits_far_and_hard);
    RUN(test_force_blast_marks_solid_terrain_without_boring_through_it);
    RUN(test_force_blast_does_not_reach_behind_a_wall);
    RUN(test_bouncing_particles_do_not_pass_through_terrain);
    RUN(test_drill_debris_settles_as_ash_without_overwriting_terrain);
    RUN(test_passing_particles_ignore_terrain);
    RUN(test_player_never_ends_a_frame_inside_solid_terrain);

    printf("\n%d tests, %d failed\n", testsRun, testsFailed);
    return testsFailed == 0 ? 0 : 1;
}
