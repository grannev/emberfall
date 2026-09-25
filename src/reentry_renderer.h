#ifndef REENTRY_RENDERER_H
#define REENTRY_RENDERER_H

/* The shock in front of something burning through the air.
 *
 * What a craft re-entering an atmosphere wears is not a trail but a cap: a
 * bow of glowing air standing a little ahead of its leading face, wrapping
 * back around its sides, and peeling off the rim into streamers that flow
 * past it. The GAME_EVENT_REENTRY sparks draw the tail; this draws the cap,
 * every frame, from the heat the atmosphere holds rather than from events,
 * so it swells and fades with the burn instead of blinking at the event
 * interval, and it turns the moment the thing wearing it turns.
 *
 * Presentation only. It reads the atmosphere, the player and the bodies
 * through const pointers and draws in the block language of beam_render.h;
 * the flicker comes from a quantised presentation time and a hash, never
 * from gameplay randomness.
 */

#include <raylib.h>

#include "atmosphere.h"
#include "dynamic_terrain.h"
#include "player.h"

/* Scene pass: the cap and its streamers in fire colours, after the character
   and the bodies so it stands in front of them. */
void ReentryRendererDraw(const AtmosphereSystem *atmosphere, const Player *player,
                         const DynamicTerrainSystem *terrain, Rectangle visible,
                         float time);
/* Emissive pass: the same shapes, hotter, so the cap blooms. */
void ReentryRendererDrawEmissive(const AtmosphereSystem *atmosphere,
                                 const Player *player,
                                 const DynamicTerrainSystem *terrain,
                                 Rectangle visible, float time);

#endif
