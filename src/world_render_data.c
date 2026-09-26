/* The bridge from simulation state to dirty pixel regions.
 *
 * This module owns dirty-chunk traversal and delegates material/temperature
 * conversion to material_render so static pages and detached bodies share one
 * palette path. What it hands the renderer is a plain rectangle of unlit
 * pixels, so the renderer stays free to decide how those pixels reach the GPU
 * and how they are lit once there.
 */
#include "world_render_data.h"

#include <stddef.h>

#include <raymath.h>

#include "materials.h"

#include "material_render.h"
#include "world_internal.h"

void WorldMarkRegionDirty(World *world, Rectangle region)
{
    int firstChunkX;
    int lastChunkX;
    int firstChunkY;
    int lastChunkY;
    int chunkY;

    if (world == NULL || world->dirtyChunks == NULL || region.width <= 0.0f ||
        region.height <= 0.0f) {
        return;
    }

    firstChunkX = (int)floorf(region.x / (float)WORLD_CHUNK_SIZE);
    lastChunkX = (int)floorf((region.x + region.width - 1.0f) /
                             (float)WORLD_CHUNK_SIZE);
    firstChunkY = (int)floorf(region.y / (float)WORLD_CHUNK_SIZE);
    lastChunkY = (int)floorf((region.y + region.height - 1.0f) /
                             (float)WORLD_CHUNK_SIZE);
    /* Columns wrap with the world; a region wider than it is all of it. */
    if (lastChunkX - firstChunkX + 1 > world->chunkColumns) {
        firstChunkX = 0;
        lastChunkX = world->chunkColumns - 1;
    }
    if (firstChunkY < 0) firstChunkY = 0;
    if (lastChunkY > world->chunkRows - 1) lastChunkY = world->chunkRows - 1;

    for (chunkY = firstChunkY; chunkY <= lastChunkY; ++chunkY) {
        int chunkX;

        for (chunkX = firstChunkX; chunkX <= lastChunkX; ++chunkX) {
            world->dirtyChunks[WorldChunkIndex(
                world, WorldWrapColumn(chunkX, world->chunkColumns), chunkY)] = 1u;
        }
    }
}

/* Everything the character walks through — decor and plants — is drawn a
   shade darker than the same material as solid ground, so a column reads as
   standing behind him and a brick wall as a wall. */
#define WORLD_PASSABLE_SHADE 0.88f

static Color WorldPassableShade(Color color)
{
    color.r = (unsigned char)((float)color.r * WORLD_PASSABLE_SHADE);
    color.g = (unsigned char)((float)color.g * WORLD_PASSABLE_SHADE);
    color.b = (unsigned char)((float)color.b * WORLD_PASSABLE_SHADE);
    return color;
}

/* What stands behind the cell at (x, y): the decor there, seen against the
   back wall if it is see-through; else the back wall; else the air. Kelp is
   not drawn here: it sways on the plants' layer. */
static MaterialRenderSample WorldBehind(const World *world, int x, int y,
                                        MaterialRenderSample air)
{
    CellMaterial decor = WorldDecorAt(world, x, y);
    CellMaterial wall = WorldBackWallAt(world, x, y);
    MaterialRenderSample sample;

    if (decor != MATERIAL_EMPTY && !MaterialIsFlora(decor)) {
        MaterialRenderContext around;

        around.shade = WorldShadeFor(x, y, decor);
        around.openAbove = WorldDecorAt(world, x, y - 1) != decor;
        around.openBelow = WorldDecorAt(world, x, y + 1) != decor;
        around.liquidDepth = 0;
        sample = MaterialRenderCell(decor, MaterialInitialTemperature(decor), x, y, around);
        sample.scene = WorldPassableShade(sample.scene);
        if (sample.scene.a < 255 && wall != MATERIAL_EMPTY) {
            sample = MaterialRenderOverWall(sample, MaterialRenderBackWall(wall, x, y));
        }
        return sample;
    }
    return wall != MATERIAL_EMPTY ? MaterialRenderBackWall(wall, x, y) : air;
}

void WorldPrepareVisible(World *world, Rectangle visible,
                         WorldRenderChunkVisitor visitor, void *context)
{
    Color uploadPixels[WORLD_CHUNK_SIZE * WORLD_CHUNK_SIZE];
    Color emissivePixels[WORLD_CHUNK_SIZE * WORLD_CHUNK_SIZE];
    Color floraPixels[WORLD_CHUNK_SIZE * WORLD_CHUNK_SIZE];
    Color liquidPixels[WORLD_CHUNK_SIZE * WORLD_CHUNK_SIZE];
    Color liquidGlowPixels[WORLD_CHUNK_SIZE * WORLD_CHUNK_SIZE];
    int firstVisibleColumn;
    int lastVisibleColumn;
    int firstVisibleRow;
    int lastVisibleRow;
    int chunkY;
    int liquidRun[WORLD_CHUNK_SIZE];

    if (world == NULL || world->cells == NULL || world->dirtyChunks == NULL ||
        visitor == NULL) {
        return;
    }

    /* Rebuilding costs what the player can see, not what the world is doing.
       Activity is spread over the whole map — a lava lake, a distant fire, a
       collapsing sand bank — while the camera shows a small window of it, and
       rebuilding a chunk nobody is looking at buys nothing. A skipped chunk
       keeps its dirty flag and is rebuilt on the frame it scrolls into view. */
    firstVisibleColumn = (int)floorf(visible.x / (float)WORLD_CHUNK_SIZE) - 1;
    lastVisibleColumn =
        (int)floorf((visible.x + visible.width) / (float)WORLD_CHUNK_SIZE) + 1;
    firstVisibleRow = (int)floorf(visible.y / (float)WORLD_CHUNK_SIZE) - 1;
    lastVisibleRow =
        (int)floorf((visible.y + visible.height) / (float)WORLD_CHUNK_SIZE) + 1;
    /* The visible columns may run past the seam; each is taken modulo the
       world, and a view wider than the world is the world once. */
    if (lastVisibleColumn - firstVisibleColumn + 1 > world->chunkColumns) {
        firstVisibleColumn = 0;
        lastVisibleColumn = world->chunkColumns - 1;
    }
    if (firstVisibleRow < 0) firstVisibleRow = 0;
    if (lastVisibleRow > world->chunkRows - 1) {
        lastVisibleRow = world->chunkRows - 1;
    }

    /* Rebuild only the chunks that changed. The simulation sleeps on a settled
       world, and so must the renderer. */
    for (chunkY = firstVisibleRow; chunkY <= lastVisibleRow; ++chunkY) {
        int unwrappedX;

        for (unwrappedX = firstVisibleColumn; unwrappedX <= lastVisibleColumn;
             ++unwrappedX) {
            int chunkX = WorldWrapColumn(unwrappedX, world->chunkColumns);
            size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                (size_t)chunkX;
            int minimumX;
            int maximumX;
            int minimumY;
            int maximumY;
            int width;
            int y;

            if (world->dirtyChunks[chunkIndex] == 0u) {
                continue;
            }
            minimumX = chunkX * WORLD_CHUNK_SIZE;
            maximumX = minimumX + WORLD_CHUNK_SIZE;
            minimumY = chunkY * WORLD_CHUNK_SIZE;
            maximumY = minimumY + WORLD_CHUNK_SIZE;
            if (maximumX > world->width) maximumX = world->width;
            if (maximumY > world->height) maximumY = world->height;
            width = maximumX - minimumX;

            /* Liquid over each column at the chunk's top edge, counted up
               into the chunk above, then carried down the rows: the depth a
               cell has darkened to, without a walk per cell. */
            {
                int column;

                for (column = 0; column < width; ++column) {
                    int above = 0;

                    while (above < MATERIAL_RENDER_DEPTH_CAP &&
                           minimumY - above - 1 >= 0 &&
                           MaterialIsLiquid((CellMaterial)WorldCellConst(
                                                world, minimumX + column,
                                                minimumY - above - 1)
                                                ->material)) {
                        ++above;
                    }
                    liquidRun[column] = above;
                }
            }

            for (y = minimumY; y < maximumY; ++y) {
                const Cell *row = WorldCellConst(world, minimumX, y);
                const Cell *rowAbove = y > 0 ? WorldCellConst(world, minimumX, y - 1)
                                             : NULL;
                const Cell *rowBelow = y + 1 < world->height
                                           ? WorldCellConst(world, minimumX, y + 1)
                                           : NULL;
                Color *scene = uploadPixels + (size_t)(y - minimumY) * (size_t)width;
                Color *emissive =
                    emissivePixels + (size_t)(y - minimumY) * (size_t)width;
                Color *flora = floraPixels + (size_t)(y - minimumY) * (size_t)width;
                Color *liquidFront = liquidPixels + (size_t)(y - minimumY) * (size_t)width;
                Color *liquidGlow = liquidGlowPixels + (size_t)(y - minimumY) * (size_t)width;
                MaterialRenderSample air = MaterialRenderAir(y, world->height);
                int x;

                for (x = 0; x < width; ++x) {
                    const Cell *cell = &row[x];
                    CellMaterial material = (CellMaterial)cell->material;
                    MaterialRenderSample sample;

                    if (material == MATERIAL_EMPTY) {
                        sample = WorldBehind(world, minimumX + x, y, air);
                        liquidRun[x] = 0;
                    } else {
                        MaterialRenderContext around;
                        CellMaterial above = rowAbove != NULL
                                                 ? (CellMaterial)rowAbove[x].material
                                                 : MATERIAL_EMPTY;
                        CellMaterial below = rowBelow != NULL
                                                 ? (CellMaterial)rowBelow[x].material
                                                 : MATERIAL_ROCK;

                        around.shade = (unsigned char)cell->shade;
                        around.openAbove = MaterialRenderOpenFace(material, above);
                        around.openBelow = MaterialRenderOpenFace(material, below);
                        around.liquidDepth = MaterialIsLiquid(material) ? liquidRun[x]
                                                                        : 0;
                        sample = MaterialRenderCell(material, cell->temperature,
                                                    minimumX + x, y, around);
                        /* See-through stuff in front of a wall is seen
                           against the wall, not against the backdrop. */
                        if (sample.scene.a < 255) {
                            MaterialRenderSample behind =
                                WorldBehind(world, minimumX + x, y, air);

                            if (behind.scene.a == 255) {
                                sample = MaterialRenderOverWall(sample, behind);
                            }
                        }
                        if (MaterialIsLiquid(material)) {
                            if (liquidRun[x] < MATERIAL_RENDER_DEPTH_CAP) {
                                ++liquidRun[x];
                            }
                        } else {
                            liquidRun[x] = 0;
                        }
                    }

                    flora[x] = BLANK;
                    if (MaterialIsFlora(material)) {
                        /* The plant goes to its own layer, and what stands
                           behind it takes its place here. */
                        float sway = MaterialRenderSway(material,
                                                        (unsigned char)cell->shade);

                        flora[x] = WorldPassableShade(sample.scene);
                        flora[x].a = (unsigned char)(MATERIAL_RENDER_FLORA_ALPHA +
                                                     (unsigned char)(sway * 127.0f));
                        sample = WorldBehind(world, minimumX + x, y, air);
                    } else {
                        CellMaterial decor = WorldDecorAt(world, minimumX + x, y);

                        /* Kelp is decor, and sways on the plants' layer; the
                           water in front of it is drawn over that layer. */
                        if (MaterialIsFlora(decor) &&
                            (material == MATERIAL_EMPTY || MaterialIsLiquid(material))) {
                            MaterialRenderContext around = {0};
                            Color plant;

                            around.shade = WorldShadeFor(minimumX + x, y, decor);
                            plant = MaterialRenderCell(decor, 20.0f, minimumX + x, y, around).scene;
                            flora[x] = WorldPassableShade(plant);
                            flora[x].a = (unsigned char)(MATERIAL_RENDER_FLORA_ALPHA +
                                                         (unsigned char)(MaterialRenderSway(decor, around.shade) * 127.0f));
                        }
                    }
                    liquidFront[x] = BLANK;
                    liquidGlow[x] = BLANK;
                    if (MaterialIsLiquid(material)) {
                        /* In front of whatever is in it: see-through for
                           water, nearly opaque for lava. */
                        liquidFront[x] = sample.scene;
                        liquidFront[x].a = material == MATERIAL_LAVA ? 215 : 150;
                        if (sample.emissive.r != 0 || sample.emissive.g != 0 ||
                            sample.emissive.b != 0) {
                            liquidGlow[x] = sample.emissive;
                            liquidGlow[x].a = material == MATERIAL_LAVA ? 215 : 150;
                        }
                    }
                    scene[x] = sample.scene;
                    emissive[x] = sample.emissive;
                }
            }
            /* At 16384 cells wide, uploading one full-width band for a local
               change moves tens of MiB. A 32x32 stack staging block keeps the
               source contiguous without any frame allocation and uploads only
               the chunk that was rebuilt. */
            if (visitor(context,
                        (Rectangle){(float)minimumX, (float)minimumY,
                                    (float)width, (float)(maximumY - minimumY)},
                        uploadPixels, emissivePixels, floraPixels, liquidPixels,
                        liquidGlowPixels)) {
                world->dirtyChunks[chunkIndex] = 0u;
            }
        }
    }
}
