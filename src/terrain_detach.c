/* Automatic detachment. See terrain_detach.h for the contract and the rule that
 * shapes it; this file records how the search is kept small and why the order
 * of operations is what it is.
 *
 * Cost of one call, entirely from compile-time constants and configuration:
 *
 *     regions        <= MAX_WORLD_DESTRUCTION_REGIONS
 *       x seeds read <= (damage span + 2)^2
 *       x detections <= maxCandidatesPerRegion
 *         x cells    <= maximumBodyCells
 *
 * Nothing here is a function of the world's size. The seed sweep reads one byte
 * per cell of the damaged box and is the cheap part; what actually costs is a
 * detection, which is why the number of them is capped twice over — by the
 * candidate limit and by the cell limit inside the detector.
 */
#include "terrain_detach.h"

#include <stddef.h>
#include <string.h>

#include "terrain_extraction.h"

TerrainDetachConfig TerrainDetachDefaultConfig(void)
{
    TerrainDetachConfig config;

    /* Eight cells is about the smallest piece that reads as a chunk of rock
       rather than as grit once it is tumbling. */
    config.minimumBodyCells = 8;
    /* Roughly a 100x100 block. It has been raised twice for the same reason:
       what a beam or a blast actually cuts free is bigger than the limit
       allowed, and a piece the detector refuses does not stay put, it hangs in
       the air. At 10240 the piece of cliff a player can cut loose and shove is
       a slab the size of a building. Still inside MAX_TERRAIN_BODY_CELLS, and
       still the bound on how far a detection search may walk — which is what
       makes it the most expensive number in this file. */
    config.maximumBodyCells = 10240;
    config.maxCandidatesPerRegion = 24;
    config.maxExtractionsPerTick = 4;
    return config;
}

void TerrainDetachInit(TerrainDetachSystem *system)
{
    if (system == NULL) {
        return;
    }
    system->config = TerrainDetachDefaultConfig();
    TerrainDetachResetStats(system);
}

void TerrainDetachResetStats(TerrainDetachSystem *system)
{
    if (system == NULL) {
        return;
    }
    {
        TerrainDetachStats empty = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

        system->stats = empty;
    }
}

/* The largest window the detector will accept, centred on the damage and slid
   — never shrunk — to sit inside the world.
 *
 * Sliding rather than clipping is what keeps the window's size a constant. A
 * clipped window near the map edge would be smaller exactly where the ground is
 * thickest, so the same blast would detach a fragment in open country and fail
 * to in a corner, for no reason a player could ever see. */
static bool DetachSearchRegion(const World *world,
                               const WorldDestructionRegion *damage,
                               Rectangle *region)
{
    int spanX = world->width < TERRAIN_DETACH_SEARCH_SPAN ? world->width
                                                          : TERRAIN_DETACH_SEARCH_SPAN;
    int spanY = world->height < TERRAIN_DETACH_SEARCH_SPAN ? world->height
                                                           : TERRAIN_DETACH_SEARCH_SPAN;
    int firstX;
    int firstY;

    /* Damage wider than the window cannot be covered by one search, and a
       partial look would answer about a component the caller never asked
       about. Refuse it: the terrain stays static, which is always allowed. */
    if (damage->maximumX - damage->minimumX + 1 > spanX ||
        damage->maximumY - damage->minimumY + 1 > spanY) {
        return false;
    }

    firstX = (damage->minimumX + damage->maximumX) / 2 - spanX / 2;
    firstY = (damage->minimumY + damage->maximumY) / 2 - spanY / 2;
    if (firstX < 0) firstX = 0;
    if (firstY < 0) firstY = 0;
    if (firstX > world->width - spanX) firstX = world->width - spanX;
    if (firstY > world->height - spanY) firstY = world->height - spanY;

    *region = (Rectangle){(float)firstX, (float)firstY, (float)spanX, (float)spanY};
    return true;
}

/* The coverage bitmap: ground some earlier search in this region already walked.

   Marking what a *failed* search explored is what keeps the worst case cheap.
   Damage cut into a hillside offers dozens of seeds that all belong to the same
   anchored mass, and without this each one would re-walk it to the cell limit.
   With it the mass is walked once and every later seed inside it is a bit test.

   It is only ever used to skip work, never to decide that something is
   detached, so a stale or missing bit costs a repeated search and nothing
   else. */
static void DetachClearCoverage(TerrainDetachSystem *system, int spanX, int spanY)
{
    memset(system->covered, 0,
           (size_t)(((spanX * spanY) + 31) / 32) * sizeof(*system->covered));
}

static int DetachCoverageIndex(int x, int y, int firstX, int firstY, int spanX)
{
    return (y - firstY) * spanX + (x - firstX);
}

static bool DetachCovered(const TerrainDetachSystem *system, int index)
{
    return (system->covered[index >> 5] & (1u << (index & 31))) != 0u;
}

static void DetachMarkCovered(TerrainDetachSystem *system, int index)
{
    system->covered[index >> 5] |= 1u << (index & 31);
}

/* Marks everything the search reached, whatever it concluded. `exploredCells`
   is a partial exploration and never a component; it is used here only as "this
   ground has been looked at". */
static void DetachMarkExplored(TerrainDetachSystem *system,
                               WorldComponentResult component, int firstX,
                               int firstY, int spanX, int spanY)
{
    int index;

    for (index = 0; index < component.exploredCells; ++index) {
        int x = (int)system->workspace.cellX[index];
        int y = (int)system->workspace.cellY[index];

        if (x < firstX || y < firstY || x >= firstX + spanX ||
            y >= firstY + spanY) {
            continue;
        }
        DetachMarkCovered(system, DetachCoverageIndex(x, y, firstX, firstY, spanX));
    }
}

/* Counts one rejection and reports whether the component may be extracted. */
static bool DetachAccepts(TerrainDetachSystem *system,
                          WorldComponentResult component)
{
    switch (component.status) {
    case WORLD_COMPONENT_DETACHED:
        break;
    case WORLD_COMPONENT_ANCHORED:
        ++system->stats.autoDetachRejectedAnchored;
        return false;
    case WORLD_COMPONENT_UNKNOWN:
        ++system->stats.autoDetachRejectedUnknown;
        return false;
    case WORLD_COMPONENT_TOO_LARGE:
        ++system->stats.autoDetachRejectedTooLarge;
        return false;
    default:
        /* INVALID. The seed stopped being solid, or the region was refused.
           Neither is a policy decision and neither is worth its own counter. */
        return false;
    }

    if (component.cellCount < system->config.minimumBodyCells) {
        ++system->stats.autoDetachRejectedTooSmall;
        return false;
    }
    /* Reachable because the detector is allowed one cell more than policy
       permits: a component of exactly maximumBodyCells + 1 comes back proven
       detached and is refused here. The two limits answer different questions —
       the detector's bounds the work a search may do, this one bounds the body
       a fragment is allowed to become — and keeping them separate is what stops
       a change to either from silently becoming a change to both. */
    if (component.cellCount > system->config.maximumBodyCells) {
        ++system->stats.autoDetachRejectedTooLarge;
        return false;
    }
    return true;
}

/* One damaged region: sweep it for seeds, prove what can be proven, extract.
   Returns bodies created. */
static int DetachProcessRegion(TerrainDetachSystem *system, World *world,
                               DynamicTerrainSystem *terrain,
                               GameEventBuffer *events,
                               const WorldDestructionRegion *damage,
                               int extractionBudget)
{
    Rectangle region;
    int created = 0;
    int checks = 0;
    int regionX;
    int regionY;
    int spanX;
    int spanY;
    int firstX;
    int firstY;
    int lastX;
    int lastY;
    int y;

    if (!DetachSearchRegion(world, damage, &region)) {
        ++system->stats.regionsRefused;
        return 0;
    }
    ++system->stats.regionsProcessed;
    regionX = (int)region.x;
    regionY = (int)region.y;
    spanX = (int)region.width;
    spanY = (int)region.height;
    DetachClearCoverage(system, spanX, spanY);

    /* Seeds are the solid cells of the damaged box grown by one. Grown, because
       what comes loose is what was *next to* the material that went away; and
       the whole box rather than only its rim, because a blast that carves a
       ring leaves an island in the middle of its own bounds, and a rim walk
       would step straight over it. Reading a cell is a byte, so sweeping the
       box is the cheap half of this function. */
    firstX = damage->minimumX - 1;
    firstY = damage->minimumY - 1;
    lastX = damage->maximumX + 1;
    lastY = damage->maximumY + 1;
    if (firstX < 0) firstX = 0;
    if (firstY < 0) firstY = 0;
    if (lastX > world->width - 1) lastX = world->width - 1;
    if (lastY > world->height - 1) lastY = world->height - 1;

    for (y = firstY; y <= lastY; ++y) {
        int x;

        for (x = firstX; x <= lastX; ++x) {
            WorldComponentResult component;
            TerrainExtractResult extracted;

            if (checks >= system->config.maxCandidatesPerRegion ||
                created >= extractionBudget) {
                return created;
            }
            if (!WorldMaterialIsSolid(WorldGetCell(world, x, y))) {
                continue;
            }
            if (DetachCovered(system, DetachCoverageIndex(x, y, regionX, regionY,
                                                          spanX))) {
                continue;
            }
            ++system->stats.detachCandidates;

            /* One cell past what policy will accept. That single argument is
               what keeps a seed in the main landmass cheap — the search stops
               there instead of exploring until the region is full — and the
               extra cell is what makes "too large" a decision this module
               makes rather than one the detector makes for it. */
            component = WorldFindComponent(world, &system->workspace, region, x, y,
                                           system->config.maximumBodyCells + 1);
            ++checks;
            ++system->stats.detachChecks;
            system->stats.detachCellsExplored += component.exploredCells;
            DetachMarkExplored(system, component, regionX, regionY, spanX, spanY);
            if (!DetachAccepts(system, component)) {
                continue;
            }

            /* The ordinary atomic path, with no shortcut around it. Every
                budget of EF-DYN-010 is checked inside, and a refusal there
                leaves the world byte-for-byte unchanged. */
            extracted = TerrainExtractComponent(world, terrain, &system->workspace,
                                                component);
            if (extracted.status != TERRAIN_EXTRACT_OK) {
                ++system->stats.autoDetachRejectedBudget;
                continue;
            }
            ++created;
            ++system->stats.autoDetachSucceeded;
            system->stats.autoDetachCells += extracted.cellCount;
            if (events != NULL) {
                const TerrainBody *body = DynamicTerrainGetConst(terrain,
                                                                 extracted.body);

                (void)GameEventsPush(events, (GameEvent){
                    .type = GAME_EVENT_TERRAIN_DETACHED,
                    .position = body != NULL ? body->position
                                             : (Vector2){(float)x, (float)y},
                    .count = extracted.cellCount,
                });
            }
            /* The sweep continues from the next cell against the world as it is
               now. Nothing about the extracted component is carried forward:
               its cells are gone, so they cannot be seeded again, and no other
               component's connectivity changed — a proven-detached component
               has no solid neighbour, so removing it cannot disconnect anything
               that was not already part of it. */
        }
    }
    return created;
}

int TerrainDetachProcess(TerrainDetachSystem *system, World *world,
                         DynamicTerrainSystem *terrain, GameEventBuffer *events)
{
    int created = 0;
    int index;

    if (system == NULL || world == NULL || world->cells == NULL ||
        terrain == NULL) {
        return 0;
    }
    /* Regions are consumed in the order the world recorded them, and the log is
       emptied whether or not anything came of it. Leaving a region behind would
       make the next call's work depend on how many ticks the last frame
       happened to run. */
    for (index = 0; index < world->destructionCount; ++index) {
        if (created < system->config.maxExtractionsPerTick) {
            created += DetachProcessRegion(system, world, terrain, events,
                                           &world->destruction[index],
                                           system->config.maxExtractionsPerTick -
                                               created);
        }
    }
    WorldClearDestruction(world);
    return created;
}
