#ifndef WORLD_H
#define WORLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <raylib.h>

#include "rng.h"

#define WORLD_CHUNK_SIZE 32
/* Light is solved on a coarser grid than the cells. Eight divides the chunk
   size, so every light cell belongs to exactly one chunk and the dirty-chunk
   bookkeeping stays exact. The field is smooth and sampled bilinearly, so a
   finer grid buys no visible detail and costs four times the solve. */
#define WORLD_LIGHT_SCALE 8

typedef enum CellMaterial {
    MATERIAL_EMPTY = 0,
    MATERIAL_DIRT,
    MATERIAL_ROCK,
    MATERIAL_SAND,
    MATERIAL_WATER,
    MATERIAL_LAVA,
    MATERIAL_STEAM,
    MATERIAL_SMOKE,
    MATERIAL_FIRE,
    MATERIAL_ASH,
    MATERIAL_ICE,
    /* Flora. Static solids that grow on a surface rather than fall onto it:
       they hold their shape like rock, weigh almost nothing, and burn. Four
       rather than one because what grows on a surface is most of what tells the
       player which biome they are standing in — a dune with a pine on it is not
       a dune. */
    MATERIAL_WOOD,
    MATERIAL_LEAF,
    MATERIAL_GRASS,
    MATERIAL_CACTUS,
    /* What ground becomes when it can no longer hold itself up: a ceiling
       that spans more than its material can bear crumbles into this, and it
       falls and piles like sand. See terrain_stability.h. */
    MATERIAL_RUBBLE,
    /* What the generator builds and the deep world is made of. Brick is laid
       masonry: the ruins, the dungeons, the crypts. Basalt is the dark rock of
       the deepest band, harder and hotter-melting than rock. Crystal grows in
       the grottos down there and glows. Snow lies on the peaks and on the
       frost, and falls and piles like sand. Fungus is the glowing cap of the
       mushrooms that grow in the dark caverns. */
    MATERIAL_BRICK,
    MATERIAL_BASALT,
    MATERIAL_CRYSTAL,
    MATERIAL_SNOW,
    MATERIAL_FUNGUS,
    /* What those who came before built with. Metal is hull plating: the
       wrecks, the outposts, the mine workings, heavy and slow to heat.
       Lumen is a lit panel set into it, cold light the colour of a screen.
       Relic is the pale dressed stone of the precursor gateways, vaults and
       platforms, cut in great blocks with a groove carved along its bands. */
    MATERIAL_METAL,
    MATERIAL_LUMEN,
    MATERIAL_RELIC,
    /* The pale rock the dunes lie on. Generated sand is a blanket over it,
       and wherever a grain would have nothing under it the grain is laid as
       limestone instead: sand that is generated already falling costs the
       whole desert a simulation the moment it is streamed into play. */
    MATERIAL_LIMESTONE,
    MATERIAL_COUNT
} CellMaterial;

/* Large horizontal generation regions. Biomes choose terrain shape and
   material composition; material physics remains defined by CellMaterial and
   the single material table. */
typedef enum WorldBiome {
    WORLD_BIOME_TEMPERATE = 0,
    WORLD_BIOME_DUNES,
    WORLD_BIOME_FROST,
    WORLD_BIOME_VOLCANIC,
    WORLD_BIOME_OCEAN,
    WORLD_BIOME_COUNT
} WorldBiome;

/* Fourteen million of these exist, so every byte here is 13.5 MiB on the
   production map and the layout is a memory decision before it is anything
   else. Fields are ordered widest first so the struct packs to 12 bytes with a
   single byte of tail padding. */
typedef struct Cell {
    /* Meaningful only for a material. An empty cell has no temperature: the
       field is whatever it was, usually zero, and every reader treats empty as
       ambient. That is deliberate — it is what lets an untouched sky cost
       nothing, because a zeroed cell is then an empty cell at rest and never
       has to be written to become one. */
    float temperature;
    /* Both stamps are compared for equality against a world counter and are
       therefore only meaningful modulo their own width. Sixteen bits cost 8 MiB
       less than thirty-two and the failure they admit is bounded and tiny: a
       cell whose stamp happens to equal the truncated counter is skipped by
       exactly one tick, or missed by exactly one effect, and behaves normally
       again immediately afterwards. For `updatedTick` that requires a cell to
       have sat awake and untouched for exactly a multiple of 65 536 ticks —
       eighteen minutes — and costs it one frame of falling. The counters skip
       the value zero precisely so that never-written cells, which are the one
       population large enough for this to be visible, can never collide. */
    uint16_t updatedTick;
    uint16_t effectStamp;
    /* Age of the temporary materials: fire, smoke and steam. The longest life
       any of them has is 420 ticks. Liquids, which have no age, use it to
       count how long a surface grain has been wandering without finding
       anywhere to fall. */
    uint16_t lifetime;
    /* MATERIAL_COUNT is deliberately kept below 256. Storing the enum as an
       int wasted four bytes in every cell; on the 16384-wide world that was
       about 54 MiB for no gameplay value. */
    uint8_t material;
    /* Ticks for which a capped heat source — lava — is holding this cell.
       The source sets it on every neighbour it touches; the neighbour's own
       thermal step counts it down and skips its cooling while it lasts. This
       is what lets a lava lake's rock lining come to rest: without it the
       lining was heated three degrees and cooled four in alternate steps
       forever, a sawtooth that kept every chunk around every lava pocket on
       the map awake for the whole session. Held at the cap, the lining's
       temperature stops changing, and a cell whose temperature does not
       change is a cell that lets its chunk sleep.

       Two ticks rather than one because the traversal direction alternates:
       a cell beside its source in the same row is visited before the source
       on one tick and after it on the next, so a one-tick flag was consumed
       twice in a row and the cell cooled in the gap. Lives in what was the
       struct's padding byte, so it costs nothing. Two bits: it never holds
       more than WORLD_HEAT_HOLD_TICKS. */
    uint8_t heatHeld : 2;
    /* The cell's own tone, 0..63, given when its material is written and
       carried with it by every move, so a grain of sand keeps its colour as
       it falls instead of flickering through the colours of the places it
       passes. What the renderer's palette pattern reads (material_render.h);
       the simulation never does. Lives in the six bits heatHeld does not
       need, so the cell does not grow. */
    uint8_t shade : 6;
} Cell;

_Static_assert(MATERIAL_COUNT <= UINT8_MAX, "Cell.material no longer fits in uint8_t");
_Static_assert(sizeof(Cell) == 12, "Cell layout grew; recheck large-world memory");

#define MAX_WORLD_REACTIONS 64

typedef struct WorldReactionEvent {
    Vector2 position;
} WorldReactionEvent;

/* Regions a destructive operation cut solid material out of, since the log was
   last cleared.

   The world records these and nothing more. It does not know that terrain
   bodies exist and must not learn: this is a fact about the world itself —
   "structural material was removed here" — and it is the only thing the
   cellular simulation contributes to automatic detachment.

   Only explicit destructive operations write here. Ordinary simulation does
   not: sand that falls has not cut anything, and a connectivity search after
   every settled grain is exactly the full-world scan this design exists to
   avoid. */
#define MAX_WORLD_DESTRUCTION_REGIONS 8

/* The largest box the log will aggregate into one entry. Two cuts far enough
   apart stay two entries rather than becoming one box with untouched ground in
   the middle. Whoever consumes the log has to be able to look at a whole entry
   plus some ground around it, so this is deliberately smaller than any sensible
   search window; terrain_detach.c asserts the relation it needs. */
#define WORLD_DESTRUCTION_MAX_SPAN 96

/* Inclusive cell bounds. Ints rather than a Rectangle because these are cells,
   not a drawing area, and rounding a region is how an off-by-one becomes a
   wrong answer about what is connected to what. */
typedef struct WorldDestructionRegion {
    int minimumX;
    int minimumY;
    int maximumX;
    int maximumY;
} WorldDestructionRegion;

typedef struct LaserResult {
    Vector2 position;
    CellMaterial material;
    bool hit;
} LaserResult;

/* ---- momentum in a liquid ----------------------------------------------
 *
 * A liquid cell has no velocity of its own — the cell did not grow to hold
 * one, and fourteen million of them would have paid for a field a few hundred
 * ever use. What it can have is an impulse: an entry in this bounded queue
 * that pushes the cell one step along a direction every tick for a number of
 * ticks, moves with it, and hands itself to the liquid it runs into. A blast
 * or a diving body moves water for a while instead of teleporting it once,
 * and when the queue is empty the liquid costs what it always did.
 *
 * Fixed capacity, counted refusals. Entries advance in queue order and the
 * order is a function of the state alone, so a replay is a replay. */
#define MAX_WORLD_FLUID_IMPULSES 32768

typedef struct WorldFluidImpulse {
    int32_t x;
    int32_t y;
    int8_t directionX;
    int8_t directionY;
    /* Steps left. */
    uint8_t strength;
    /* Steps taken per tick, at least one. A spray thrown by a supersonic
       pass moves several cells a tick; a ripple moves one. Still 12 bytes. */
    uint8_t pace;
} WorldFluidImpulse;

/* ---- frost --------------------------------------------------------------
 *
 * Ice made by the cryo beam spreads through the water it touches: every cell
 * frozen puts its liquid neighbours on this queue, and each tick a bounded
 * number of them freeze in turn, as long as the beam keeps paying for it.
 * That is what makes a pond freeze over from where the beam lands rather
 * than cell by cell under the beam alone, and stop at the pond's edge — rock
 * is not water and never joins the queue. Fixed capacity, refusals counted;
 * a frontier wider than the queue freezes the cells it holds and the
 * neighbours of those join as they go. */
#define MAX_WORLD_FROST 1024

typedef struct WorldFrostEntry {
    int32_t x;
    int32_t y;
} WorldFrostEntry;

typedef struct WorldFluidStats {
    /* Live entries, and refusals since the last WorldInit. */
    int impulsesActive;
    int impulsesRefused;
    /* Water the drill turned to steam, since the last WorldInit. */
    int vaporised;
    /* The frost frontier: cells waiting, cells the beam has paid for, and
       what it has frozen since the last WorldInit. */
    int frostQueued;
    int frostRefused;
    float frostBudget;
    /* Ticks since the beam last paid; the frontier is dropped after sixty. */
    int frostIdle;
    int frozen;
    /* Refreshed by every tick. */
    int impulseMoves;
    int lifts;
    int headChanges;
} WorldFluidStats;

/* Work performed by the most recent fixed simulation tick. These counters are
   deliberately structural rather than time-based: they stay meaningful across
   machines and make performance regressions testable without flaky deadlines. */
typedef struct WorldTickStats {
    uint64_t processedCells;
    uint32_t processedChunks;
} WorldTickStats;

/* Cells per side of one back-wall block. */
#define WORLD_BACK_WALL_SCALE 4
/* Blocks per side of the window the back layer's hold is checked in. A part
   of the layer that reaches the window's edge is taken to be held: like the
   terrain's own detach check, this never asks about the whole world. */
#define WORLD_BACK_WALL_WINDOW 128
/* Blocks per side of one falling piece of back wall. */
#define WORLD_BACK_WALL_PIECE 4

typedef struct WorldBackWallPiece {
    /* Top-left cell. */
    int x;
    int y;
    /* Bit (by * WORLD_BACK_WALL_PIECE + bx) for each block it carries. */
    uint16_t mask;
    uint8_t material;
} WorldBackWallPiece;

typedef struct World {
    int width;
    int height;
    Cell *cells;
    uint32_t tick;
    uint32_t effectSerial;
    /* The seed the current terrain was generated from, and the stream every
       later world mutation draws from. Both are part of the world's state on
       purpose: a world plus the inputs applied to it must replay identically,
       and that is impossible if an effect draws from a generator shared with
       the frame loop. */
    uint64_t seed;
    Rng rng;
    WorldTickStats lastTickStats;
    WorldReactionEvent reactions[MAX_WORLD_REACTIONS];
    int reactionCount;
    /* Destructive cuts awaiting a detach check. Aggregated on write, drained by
       whoever runs the check; `destructionDropped` counts the regions the log
       had no room for, which is a refusal to look rather than a lost mutation:
       the world is correct either way, some terrain merely stays static. */
    WorldDestructionRegion destruction[MAX_WORLD_DESTRUCTION_REGIONS];
    int destructionCount;
    int destructionDropped;
    WorldFluidImpulse fluidImpulses[MAX_WORLD_FLUID_IMPULSES];
    WorldFrostEntry frost[MAX_WORLD_FROST];
    WorldFluidStats fluid;
    int chunkColumns;
    int chunkRows;
    int activeChunkCount;
    /* The simulation schedule, kept in two representations because both are
       needed: a flag per chunk for O(1) membership, and a compact per-chunk-row
       list of active columns for iteration. Without the lists a settled world
       still walked every chunk slot of every row — 442 000 of them per tick on
       the production map — to discover it had nothing to do.

       `activeChunks` is the set being simulated and is frozen for the duration
       of a tick; `nextActiveChunks` accumulates what the tick woke. They swap
       at the end of WorldUpdate. */
    uint8_t *activeChunks;
    uint8_t *nextActiveChunks;
    int32_t *activeRowColumns;
    int32_t *activeRowCount;
    int32_t *nextRowColumns;
    int32_t *nextRowCount;
    /* True only inside WorldUpdate. A wake during a tick schedules the next
       one; a wake between ticks — a laser, a settling particle, a drilled
       tunnel — schedules the tick about to run. */
    bool simulating;
    /* How many water and lava cells each chunk holds, kept exact by the three
       functions that write a cell's material. The water/lava reaction used to
       scan eight neighbours of every water and lava cell every tick, and in
       almost every chunk on the map it found nothing — a lake has no lava in
       it. With the counts, a cell scans only when its own chunk, or the one
       across the border it sits on, holds the material it could react with:
       a third of a large pool's simulation, gone. */
    uint16_t *chunkWater;
    uint16_t *chunkLava;
    /* Chunks whose pixels changed since the last upload. The simulation already
       tracks where work happens; the renderer reuses that instead of rebuilding
       the whole texture every frame. */
    uint8_t *dirtyChunks;
    /* Chunks whose light inputs are stale. Separate from `dirtyChunks` because
       the two are consumed at different times: light must be refreshed once,
       wherever the terrain changed, while a pixel rebuild waits until the chunk
       is on screen and so may stay pending for many frames. Sharing one flag
       makes the light refresh re-scan every off-screen chunk every frame. */
    uint8_t *lightDirtyChunks;
    /* The back layer: what stands behind the cells, one material per block
       of WORLD_BACK_WALL_SCALE cells square, made with the world and never
       changed by play. Rock behind the ground, so a cave or a tunnel the
       player digs is a hollow in something rather than a hole in the
       picture; the builder's own wall behind a vault, a hold, an outpost.
       Presentation reads it where a cell is empty; nothing simulates it. */
    uint8_t *backWalls;
    int backWallColumns;
    int backWallRows;
    /* Workspace for the back layer's hold check, a window of
       WORLD_BACK_WALL_WINDOW blocks square: visit marks and a queue. */
    uint8_t *backWallVisit;
    int *backWallQueue;
    /* Coarse light field. `emission` and `opacity` are derived from the cells and
       refreshed only where chunks are dirty; `light` is solved from them when
       something that can change it has, and the renderer uploads the solved
       window to the GPU, where a shader lights every pixel from it. Nothing
       here is baked into a page: a lamp moving or a day turning rebuilds no
       chunk. */
    int lightColumns;
    int lightRows;
    /* Two channels, not one. A single intensity can darken but cannot colour,
       so a lava lake lit its own cavern in grey. `lightSky` is the fraction of
       full daylight reaching down from the surface, `lightEmber` is everything
       that burns, and the difference between them is what warms the light near
       a fire. */
    float *lightSky;
    float *lightEmber;
    float *lightEmission;
    float *lightOpacity;
    /* One row of light cells, for the solve to resolve a row's transmission
       into once and read for both channels. */
    float *lightScratch;
    /* Counts solves. The renderer keeps the revision its light texture was
       uploaded from, so a frame in which nothing was re-solved uploads
       nothing. */
    uint32_t lightRevision;
    /* How much daylight the sky is giving, 0 at midnight and 1 at noon. The sky
       channel is solved for full day and scaled by this where it is drawn, so
       night costs no solve: a column open to the sky simply shows less, every
       overhang with it, and the ground the sun was reaching goes as dark as the
       ground it never reached. Nothing in the simulation reads it — night
       changes what can be seen, not what happens. */
    float daylight;
    /* One movable light the caller owns, so the player can carry their own glow
       into a tunnel that has no other source. */
    Vector2 pointLight;
    float pointLightRadius;
    float pointLightStrength;
    /* State of the light the last solve was run for, so a still scene can skip
       the solve entirely. */
    Vector2 solvedPointLight;
    float solvedPointLightStrength;
    uint32_t solvedTick;
    /* The column window the last solve covered. Sky light is re-solved when the
       terrain changed or when the window moved onto columns the last solve did
       not reach; a lamp moving inside an unchanged window needs only ember. */
    int solvedFirstColumn;
    int solvedLastColumn;
    bool lightSolved;
    /* Rows of the last solve's two sweeps that were open sky and not swept:
       the downward sweep starts at the first row with anything in it, and
       the upward one stops once its ember has faded out in the air. A
       workload counter for the bench and the tests, not a timing. */
    struct {
        int skippedRows;
    } lightStats;
    /* Planes for a solve window that crosses the seam where the world wraps:
       emission, opacity, sky and ember, each window-wide, grown to the widest
       window ever solved and kept. NULL until the camera first looks across
       the seam. */
    float *lightWindow;
    size_t lightWindowCapacity;
} World;

bool WorldInit(World *world, int width, int height);
void WorldUnload(World *world);
/* Generates terrain from `seed` and stores it. The same seed always produces
   the same world, which is what makes bug reports, regression tests and
   benchmark scenarios repeatable. */
void WorldGenerate(World *world, uint64_t seed);
/* The nominal biome at a column. Terrain parameters blend near boundaries, so
   this is an identity/debug query rather than a hard material border. */
WorldBiome WorldBiomeAt(const World *world, int x);
const char *WorldBiomeName(WorldBiome biome);
Vector2 WorldPlayerSpawn(const World *world);
/* Wakes generated dynamic or heated cells inside a streamed gameplay region.
   Actual cell mutations wake themselves regardless of this region. */
void WorldActivateRegion(World *world, Rectangle region);
void WorldUpdate(World *world);
/* Position of the caller-owned light, applied on the next draw. A strength of
   zero disables it. */
void WorldSetPointLight(World *world, Vector2 position, float radius, float strength);
/* Sets how much daylight the sky gives, clamped to 0..1. Applied on the next
   solve, like the point light. */
void WorldSetDaylight(World *world, float daylight);

/* ---- altitude ------------------------------------------------------------
 *
 * The world has a top as well as a bottom. Below the cloud line everything is
 * ground and weather; above the space line there is nothing to hold anything
 * down, and between them the pull fades out rather than switching off, so a
 * body thrown hard enough leaves the sky gradually and visibly.
 *
 * Fractions of the world's height rather than fixed cells, because a test world
 * a hundred and forty cells tall has to have the same three bands as a
 * production one, in the same places relative to its ground.
 *
 * The two fractions were raised with the world's height. Re-entry is a thing
 * that happens in the band between them (`atmosphere.h`), and it has to be
 * long enough to be a descent: at the production height the corridor is over
 * a thousand cells deep and the open space above it is five hundred, so
 * leaving the atmosphere and coming back are journeys rather than a line
 * crossed twice in a second.
 */
#define WORLD_SPACE_LINE 0.14f
#define WORLD_CLOUD_LINE 0.42f

/* ---- the ground band ----------------------------------------------------
 *
 * Everything the generator lays out — surfaces, strata, caves, pockets, the
 * sea — lives in a band of at most this many rows at the bottom of the world,
 * and is described as fractions of that band. Whatever height the world has
 * above the band is sky, and only sky.
 *
 * This is what lets the world grow upward without the ground moving: raising
 * the height used to scale every surface fraction with it, so a taller world
 * was a world with deeper soil and taller hills as well as a taller sky, and
 * the previous raise had to rescale every amplitude by hand to keep a hill the
 * size it was. Now the ground is the same ground at any height at or above
 * the band, and a test world shorter than the band is simply all band, which
 * is what the tests were written against.
 *
 * The rows above the band cost nothing while they stay empty: the cell array
 * is never written there — generation writes from the surface down and an
 * empty cell has no temperature to initialise — so the pages the sky would
 * occupy are never materialised. */
#define WORLD_GROUND_ROWS 1440

static inline int WorldGroundRows(const World *world)
{
    return world->height < WORLD_GROUND_ROWS ? world->height : WORLD_GROUND_ROWS;
}

/* Rows of pure sky above the ground band: zero on a world no taller than
   the band, the whole of the extra height otherwise. */
static inline int WorldSkyRows(const World *world)
{
    return world->height - WorldGroundRows(world);
}

/* A fraction of the ground band, as a world row. */
static inline float WorldGroundY(const World *world, float fraction)
{
    return (float)WorldSkyRows(world) + (float)WorldGroundRows(world) * fraction;
}

/* Where the sea stands, as a fraction of the ground band.
 *
 * One number for the whole map rather than a property of the ocean biome: the
 * sea is the same sea wherever the coast is, and a level that varied by region
 * would put a step in the water at every boundary. It sits below the lowest
 * point any land biome's surface can reach, so a valley inland stays a valley;
 * only ground that is genuinely under it — the ocean floor and the shelf either
 * side of it — is flooded. */
#define WORLD_SEA_LEVEL 0.62f

static inline float WorldSeaLevelY(const World *world)
{
    return WorldGroundY(world, WORLD_SEA_LEVEL);
}

static inline float WorldSpaceLineY(const World *world)
{
    return (float)world->height * WORLD_SPACE_LINE;
}

static inline float WorldCloudLineY(const World *world)
{
    return (float)world->height * WORLD_CLOUD_LINE;
}

/* How much of gravity reaches `y`: one at and below the cloud line, zero at and
   above the space line, and a smooth fall between them. */
static inline float WorldGravityScaleAt(const World *world, float y)
{
    float space;
    float cloud;
    float amount;

    if (world == NULL || world->height <= 0) return 1.0f;
    space = WorldSpaceLineY(world);
    cloud = WorldCloudLineY(world);
    if (y >= cloud) return 1.0f;
    if (y <= space) return 0.0f;
    amount = (y - space) / (cloud - space);
    /* Smoothstep, so neither end has a corner in it: a body drifting up through
       the band slows its fall continuously instead of stepping. */
    return amount * amount * (3.0f - 2.0f * amount);
}

/* How much air there is at `y`: one at and below the cloud line, zero at and
   above the space line.
 *
 * The same band the gravity uses, asked as a different question, and it is a
 * question presentation needs: a painted horizon is something you can only see
 * from inside the air. Linear rather than smoothed, because what reads it fades
 * a picture rather than accelerating a body — a smoothstep here holds the
 * horizon at nearly full strength for the first third of the climb, which is
 * the part of the climb where losing it is the whole point. */
static inline float WorldAirFractionAt(const World *world, float y)
{
    float space;
    float cloud;

    if (world == NULL || world->height <= 0) return 1.0f;
    space = WorldSpaceLineY(world);
    cloud = WorldCloudLineY(world);
    if (y >= cloud) return 1.0f;
    if (y <= space) return 0.0f;
    return (y - space) / (cloud - space);
}

CellMaterial WorldGetCell(const World *world, int x, int y);
int WorldCountDynamicCells(const World *world);
/* Cells of `material` in one chunk, for water and lava; zero for anything the
   world does not count. Exposed so a test can hold the bookkeeping to the
   truth of a brute-force count. */
int WorldChunkMaterialCount(const World *world, int chunkX, int chunkY,
                            CellMaterial material);
float WorldGetTemperature(const World *world, int x, int y);
void WorldSetTemperature(World *world, int x, int y, float temperature);
bool WorldMaterialIsSolid(CellMaterial material);
/* The tone a cell of `material` written at (x, y) is given: a hash of the
   two, so generation, a phase change and a weld all agree on what a cell
   written there looks like. */
uint8_t WorldShadeFor(int x, int y, CellMaterial material);
/* A cell's own tone, for what carries it out of the world and back: the
   extraction copies it into a body so a slab keeps its colours, and a weld
   writes it back. Zero for an empty or out-of-world cell. */
uint8_t WorldGetShade(const World *world, int x, int y);
void WorldSetShade(World *world, int x, int y, uint8_t shade);
void WorldSetCell(World *world, int x, int y, CellMaterial material);

/* Notes that solid material was cut out of the given inclusive cell bounds.
   Destructive world effects call this for themselves; a caller outside the
   world module needs it only when it removes structural cells by some other
   route. Overlapping or touching regions are merged, so an operation that hits
   the same area many times in a tick costs one entry rather than many. */
void WorldRecordDestruction(World *world, int minimumX, int minimumY,
                            int maximumX, int maximumY);
/* Empties the log. Whoever consumes the regions is responsible for this;
   nothing clears them implicitly, so a tick that never runs a detach check does
   not silently discard what it was told. */
void WorldClearDestruction(World *world);

/* Gives the liquid cell at (x, y) an impulse: `strength` steps of one cell
   along (directionX, directionY), each component -1, 0 or 1. Returns false
   and counts a refusal when the cell is not liquid, the direction is zero, or
   the queue is full. The same cell may hold several. */
bool WorldPushLiquid(World *world, int x, int y, int directionX, int directionY,
                     int strength);
/* The same, moving `pace` cells a tick instead of one: the jet of a spray
   rather than the roll of a wave. `strength` is still the total number of
   steps. */
bool WorldPushLiquidFast(World *world, int x, int y, int directionX,
                         int directionY, int strength, int pace);
/* Pushes every liquid cell within `radius` of `centre` away from it, with
   `strength` steps at the centre falling to one at the edge. What a blast, a
   splash or a body entering the water does to it. Bounded by the circle. */
int WorldPushLiquidRadial(World *world, Vector2 centre, float radius,
                          int strength);
/* Throws the surface up around `centre`: every surface cell — liquid with
   nothing over it — within `radius` is pushed up and outward, hardest at the
   centre, and the liquid under it out and down. What a heavy body dropped
   into a river from a height does to the river: a crown of water thrown
   clear of the surface and a ring spreading from it. Bounded by the circle. */
int WorldSplashLiquid(World *world, Vector2 centre, float radius, int strength);
/* Moves the liquid cell at (x, y) to the first empty cell above it, looking
   up through liquid for at most `reach` rows. Returns false and leaves the
   world unchanged when the cell is not liquid or the column above it is
   sealed by a solid within reach. What a body settling on a lake bed does
   with the water it lies in: the water is lifted to the surface rather than
   destroyed, so the lake keeps every cell it had. */
bool WorldLiftLiquidOut(World *world, int x, int y, int reach);
/* Pays for `cells` more cells of the frost frontier to freeze, and starts
   the frontier from the water around (x, y) if it is not running there. The
   cryo beam calls it where it meets water; the frontier then spreads on its
   own, one bounded step a tick, until the budget is spent. */
void WorldFrostFeed(World *world, int x, int y, float cells);

void WorldDestroyCircle(World *world, int centerX, int centerY, int radius,
                        float rockToLavaChance);
int WorldDrillCircle(World *world, int centerX, int centerY, int radius);
/* The dent a heavy blow leaves, plus the fractures running out of it.
 *
 * A crater rather than a hole: the bowl is wider than it is deep, so the impact
 * reads as something enormous having struck a surface rather than as a shot
 * having been fired into it. The cracks are short deterministic rays through
 * whatever is still solid — no stress model, no propagation, just the shape a
 * player recognises as "that hit hard".
 *
 * `direction` is the way the blow was travelling; cracks favour it and the
 * surface either side of it. Everything is bounded by the radius and the crack
 * length, and the damage is logged the way every other destructive cut is. */
void WorldApplyPunch(World *world, Vector2 at, Vector2 direction, int radius,
                     int crackCount, int crackLength);
void WorldApplyShockwave(World *world, int centerX, int centerY, int innerRadius,
                         int outerRadius);
/* What an explosion leaves behind, as opposed to what it removes.
 *
 * A circle of deleted cells is a hole punched in a picture: the rim is smooth,
 * nothing around it changed, and the rock the blast did not reach looks exactly
 * as it did a moment before. This is the same event with the consequences left
 * in — a crater whose rim is torn rather than drawn, a ring of rock left glowing
 * around it, and branching fractures thrown out into the ground beyond, which
 * carry the blast's reach much further than its radius and are what make the
 * next shot land in rock that already remembers the last one.
 *
 * `rockToLavaChance` is the same molten-slag chance WorldDestroyCircle takes.
 * Everything is bounded by `coreRadius` and `crackLength`, and the damage is
 * logged the way every other destructive cut is, so what it cuts free falls. */
void WorldApplyBlast(World *world, Vector2 at, int coreRadius,
                     float rockToLavaChance, int crackCount, int crackLength);
/* One heavy blow along a cone: throws dynamic cells a long way and scours a thin
   layer off the exposed face of anything solid. `spreadCosine` is the cosine of
   the cone's half angle; `reach` is how far the nearest cells are thrown. */
void WorldApplyForceBlast(World *world, Vector2 origin, Vector2 direction,
                          float length, float spreadCosine, int reach);
/* Where the beam first meets solid material, changing nothing. Split out of
   WorldApplyLaser so a caller can find out what the beam would hit before
   deciding whether it is the beam's real target: a detached body standing in
   front of that wall has to stop the beam, and the wall behind it must not be
   burned by a shot that never reached it. */
LaserResult WorldBeamHit(const World *world, Vector2 start, Vector2 end);
LaserResult WorldApplyLaser(World *world, Vector2 start, Vector2 end, float radius,
                            float deltaTime);
/* Thermal inverse of the laser: chills everything along the ray, freezing water
   to ice and settling lava back into rock. */
LaserResult WorldApplyChill(World *world, Vector2 start, Vector2 end, float radius,
                            float deltaTime);
const char *WorldMaterialName(CellMaterial material);
/* What stands behind cell (x, y) in the back layer; MATERIAL_EMPTY for the
   open sky. Columns wrap. */
CellMaterial WorldGetBackWall(const World *world, int x, int y);
/* After something was cut out of the box: every part of the back layer near
   it that no longer touches a static solid cell anywhere comes away. It is
   removed from the layer and handed back as pieces of up to 4x4 blocks, at
   most `capacity` of them (the rest simply go). Bounded by
   WORLD_BACK_WALL_WINDOW; returns the pieces written. */
int WorldBreakBackWalls(World *world, int minimumX, int minimumY, int maximumX,
                        int maximumY, WorldBackWallPiece *pieces, int capacity);

#endif
