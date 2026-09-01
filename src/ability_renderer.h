#ifndef ABILITY_RENDERER_H
#define ABILITY_RENDERER_H

#include <raylib.h>

#include "abilities.h"
#include "player.h"

/* `player` and `time` are what glue a beam to the face it comes out of: the
   visor is recomputed here, at draw time, from the player as they are being
   drawn, rather than read from where the simulation happened to leave it. A
   beam drawn from a stored origin trails the head at speed. `time` advances the
   flicker; the same value twice draws the same beam. */
void AbilityRendererDraw(const AbilitySystem *abilities, const Player *player,
                         float time);
void AbilityRendererDrawReticle(const AbilitySystem *abilities,
                                Vector2 aimPosition);
void AbilityRendererDrawEmissive(const AbilitySystem *abilities,
                                 const Player *player, float time);

#endif
