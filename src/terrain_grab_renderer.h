#ifndef TERRAIN_GRAB_RENDERER_H
#define TERRAIN_GRAB_RENDERER_H

/* The telekinetic hold, drawn.
 *
 * A power that moves a slab across the screen with no visible link between the
 * player and the slab reads as the world glitching. What the reference shows is
 * a wide pale beam of force reaching out from the character, its edges broken
 * and outlined, ending in a white core with orange rings turning around it and
 * sparks pulled in toward the grip.
 *
 * Presentation only. It reads the interaction system and the player and writes
 * to neither; every number it draws is one the simulation already decided.
 */

#include "player.h"
#include "terrain_interaction.h"

/* `time` advances the flicker. Passing a steadily increasing value makes the
   beam alive rather than frozen; passing the same value twice draws the same
   picture, which is what lets the smoke run take a reproducible screenshot. */
void TerrainGrabRendererDrawScene(const TerrainInteractionSystem *system,
                                  const DynamicTerrainSystem *terrain,
                                  const Player *player, float time);
void TerrainGrabRendererDrawEmissive(const TerrainInteractionSystem *system,
                                     const DynamicTerrainSystem *terrain,
                                     const Player *player, float time);

#endif
