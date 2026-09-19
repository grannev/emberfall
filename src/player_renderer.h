#ifndef PLAYER_RENDERER_H
#define PLAYER_RENDERER_H

#include <raylib.h>

#include "player.h"

void PlayerRendererDraw(const Player *player, Vector2 aimPosition);
/* The same figure in opaque black, for the emissive plane: the character has
   a body, and what glows behind that body — a star, when they fly into space
   — must not bloom through it. Drawn before the character's own glow. */
void PlayerRendererDrawSilhouette(const Player *player, Vector2 aimPosition);
void PlayerRendererDrawEmissive(const Player *player);

#endif
