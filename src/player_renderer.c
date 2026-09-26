#include "player_renderer.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

/* ---- Character rendering -------------------------------------------------
 *
 * The figure is built in a body frame rather than as a fixed sprite: `up` runs
 * from the hips to the head and `side` across the shoulders, and the whole frame
 * rotates from vertical toward the direction of travel as `leanAmount` rises.
 * Hovering, the character stands in the air with his knees drawn back; at speed
 * the same joints lay out flat with the arms thrown forward, because the frame
 * turned rather than because a different sprite was chosen.
 *
 * There is no outline. A dark rim around every limb flattens the figure into a
 * silhouette — a brick with a cape — and hides the shading that makes it read as
 * a body. Contrast comes instead from a lit tone on the side facing `up` and a
 * shadow tone opposite it, so the character is legible against terrain that the
 * lighting has already darkened.
 */

typedef struct BodyFrame {
    Vector2 origin;
    Vector2 up;
    Vector2 side;
} BodyFrame;

/* The block language the rest of the effects use. A perfect ring drawn over a
   world of cells is the single most obvious thing in the frame that does not
   belong to it. */
static float PlayerFxNoise(int a, int b)
{
    unsigned int h = (unsigned int)a * 374761393u ^ (unsigned int)b * 668265263u;

    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

static void PlayerFxBlock(float x, float y, float block, Color color)
{
    DrawRectangleV((Vector2){floorf(x / block) * block,
                             floorf(y / block) * block},
                   (Vector2){block, block}, color);
}

/* Separate embers on a circle rather than a drawn curve, with gaps. */
static void PlayerFxRing(Vector2 centre, float radius, float block, Color color)
{
    int count = (int)(radius * 6.283185f / (block * 1.4f));
    int index;

    if (radius <= 0.0f || count <= 0) {
        return;
    }
    if (count > 160) count = 160;
    for (index = 0; index < count; ++index) {
        float angle = (float)index / (float)count * 6.283185f;
        float wobble = 1.0f + (PlayerFxNoise(index, (int)radius) - 0.5f) * 0.2f;

        if (PlayerFxNoise(index, (int)radius + 31) < 0.3f) {
            continue;
        }
        PlayerFxBlock(centre.x + cosf(angle) * radius * wobble,
                      centre.y + sinf(angle) * radius * wobble, block, color);
    }
}

static void PlayerFxBlob(Vector2 centre, float radius, float block, Color color)
{
    float y;

    for (y = -radius; y <= radius; y += block) {
        float x;

        for (x = -radius; x <= radius; x += block) {
            if (sqrtf(x * x + y * y) > radius) {
                continue;
            }
            PlayerFxBlock(centre.x + x, centre.y + y, block, color);
        }
    }
}

/* Body units into cells. Every length in this file that is a part of the
   character passes through it, so the figure has exactly one size and the
   proportions between its parts cannot drift when that size changes. */
#define BODY(units) ((units) * PLAYER_BODY_SCALE)

static Vector2 BodyPoint(const BodyFrame *frame, float alongUp, float alongSide)
{
    return (Vector2){
        frame->origin.x + frame->up.x * BODY(alongUp) +
            frame->side.x * BODY(alongSide),
        frame->origin.y + frame->up.y * BODY(alongUp) +
            frame->side.y * BODY(alongSide)
    };
}

static void DrawBodyCell(Vector2 point, int size, Color color)
{
    /* Half-cell character pixels allow adult facial/limb proportions without
       enlarging the hero relative to the world or changing the collider. */
    float width = (float)size * 0.5f;
    DrawRectangleV((Vector2){floorf((point.x - width * 0.5f) * 2.0f) * 0.5f,
                             floorf((point.y - width * 0.5f) * 2.0f) * 0.5f},
                   (Vector2){width, width}, color);
}

/* Fills a rectangle of the body frame one cell at a time, sampled at half a cell
   so a turned frame leaves no holes between the samples. Tone is chosen per
   column, which is what shades the body without an outline around it. */
static void FillBodyRect(const BodyFrame *frame, float fromUp, float toUp,
                         float halfWidth, Color shadow, Color mid, Color lit)
{
    float alongUp;

    for (alongUp = fromUp; alongUp <= toUp + 0.001f; alongUp += 0.25f) {
        float alongSide;

        for (alongSide = -halfWidth; alongSide <= halfWidth + 0.001f;
             alongSide += 0.25f) {
            Color tone = alongSide > 0.4f ? lit : (alongSide < -0.4f ? shadow : mid);

            DrawBodyCell(BodyPoint(frame, alongUp, alongSide), 1, tone);
        }
    }
}

/* A limb as a stepped run of cells. Limbs have to bend to arbitrary angles while
   the torso and head stay axis-aligned blocks; drawing them as rotated
   rectangles would shear them, and as fixed sprites they could not bend at all. */
static void DrawLimb(Vector2 from, Vector2 to, int thickness, Color color)
{
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float span = fmaxf(fabsf(dx), fabsf(dy));
    /* Two samples per cell, so a limb at any angle stays a solid run. */
    int steps = (int)ceilf(span * 4.0f);
    int step;

    if (steps < 1) {
        steps = 1;
    }
    for (step = 0; step <= steps; ++step) {
        float amount = (float)step / (float)steps;

        DrawBodyCell((Vector2){from.x + dx * amount, from.y + dy * amount},
                     thickness, color);
    }
}

#define SHOULDER_UP PLAYER_SHOULDER_UP

/* Thigh and shin, in body units: a leg straight down from the hips at -0.6
   would reach 6.8 below them, a little past the soles at -7, so a standing
   leg is always a touch bent and never locks. */
#define LEG_UPPER 3.4f
#define LEG_LOWER 3.4f

/* Two-bone reach from `hip` to `foot` in body-frame units, the knee bending
   toward +x — forward, since the frame's side axis is turned to face the way
   the character looks. A foot out of reach is drawn at full stretch. */
static Vector2 PlayerKnee(Vector2 hip, Vector2 foot)
{
    float dx = foot.x - hip.x;
    float dy = foot.y - hip.y;
    float length = sqrtf(dx * dx + dy * dy);
    float distance = length;
    float along;
    float out;
    Vector2 across;

    if (length < 0.001f) {
        return (Vector2){hip.x + LEG_UPPER, hip.y};
    }
    if (distance > LEG_UPPER + LEG_LOWER - 0.01f) {
        distance = LEG_UPPER + LEG_LOWER - 0.01f;
    }
    along = (LEG_UPPER * LEG_UPPER - LEG_LOWER * LEG_LOWER + distance * distance) /
            (2.0f * distance);
    out = sqrtf(fmaxf(0.0f, LEG_UPPER * LEG_UPPER - along * along));
    dx /= length;
    dy /= length;
    across = (Vector2){-dy, dx};
    if (across.x < 0.0f) {
        across = (Vector2){dy, -dx};
    }
    return (Vector2){hip.x + dx * along + across.x * out,
                     hip.y + dy * along + across.y * out};
}

/* A leg in world space: a thigh two cells thick, a shin the same with a
   knee pad where they meet, and a boot three cells long pointing the way
   the character faces. Two cells of limb at this scale read as a leg; one
   read as a drawn line. */
static void PlayerDrawLeg(const BodyFrame *frame, Vector2 hip, Vector2 knee,
                          Vector2 foot, Color thigh, Color shin, Color boot)
{
    DrawLimb(hip, knee, 3, thigh);
    DrawLimb(knee, foot, 2, shin);
    DrawBodyCell(foot, 2, boot);
    DrawBodyCell((Vector2){foot.x + frame->side.x * 0.7f,
                           foot.y + frame->side.y * 0.7f}, 2, boot);
}

/* Where the feet go on foot, in body-frame units (x forward, y up from the
   hips). On the ground the two feet run half a cycle apart: each is planted
   and swept back under the body for half the cycle and lifted and carried
   forward for the other, the sweep a quarter stride either way so a planted
   foot reads as planted. At sprint speed the reach is capped to preserve
   anatomy, and cadence takes over. In the air the legs gather — a knee up on
   the way up, reaching for the ground on the way down. */
static void PlayerWalkFeet(const Player *player, float facing, float ground,
                           Vector2 *lead, Vector2 *trail)
{
    float moving = Clamp(fabsf(player->velocity.x) / 20.0f, 0.0f, 1.0f);
    /* A literal quarter of the distance travelled in a cycle made a sprint
       an implausible split-legged lunge at this sprite scale. Keep the feet
       under the hips and let cadence carry the extra ground speed. */
    float run = Clamp((fabsf(player->velocity.x) - PLAYER_WALK_SPEED) /
                       (PLAYER_RUN_SPEED - PLAYER_WALK_SPEED), 0.0f, 1.0f);
    float reach = fminf(PlayerStride(player) * 0.25f / PLAYER_BODY_SCALE,
                        2.55f + 0.7f * run);
    /* Running backward — facing the cursor, moving away from it — plays the
       cycle in reverse, so the planted foot still sweeps with the ground. */
    float direction = player->velocity.x * facing >= 0.0f ? 1.0f : -1.0f;
    float phase = player->walkPhase * 2.0f * PI;
    float lift = 1.05f + 1.0f * run;
    int leg;

    if (!player->grounded) {
        if (player->velocity.y < 0.0f) {
            *lead = (Vector2){1.6f, ground + 2.6f};
            *trail = (Vector2){-1.0f, ground + 0.6f};
        } else {
            *lead = (Vector2){0.9f, ground + 0.8f};
            *trail = (Vector2){-0.7f, ground + 1.4f};
        }
        return;
    }
    for (leg = 0; leg < 2; ++leg) {
        float at = phase + (leg == 0 ? 0.0f : PI);
        /* Forward when the swing carries it forward: the lifted half. */
        float swing = sinf(at) * reach * direction;
        float raised = fmaxf(0.0f, cosf(at)) * lift;
        Vector2 stand = {leg == 0 ? 1.0f : -0.85f, ground};
        Vector2 step = {swing, ground + raised};
        Vector2 foot = {stand.x + (step.x - stand.x) * moving,
                        stand.y + (step.y - stand.y) * moving};

        if (leg == 0) {
            *lead = foot;
        } else {
            *trail = foot;
        }
    }
}

/* Where the hands reach, in body-frame coordinates: x across the shoulders, y
   from the hips toward the head. Absolute rather than relative to some offset,
   because a hand placed by an unexplained constant is a hand nobody can move
   with confidence later. */
static void PlayerHandTargets(const Player *player, Vector2 aimLocal,
                              Vector2 pushLocal, float lean, float wave,
                              Vector2 *lead, Vector2 *trail)
{
    /* Hanging at rest — the hands sit below the shoulders — and thrown out past
       the head at speed, along the body axis, which at full lean is the
       direction of travel. */
    Vector2 restLead = {2.1f - 1.0f * lean, 0.1f + 8.5f * lean};
    Vector2 restTrail = {-2.0f + 0.5f * lean, -0.3f + 2.4f * lean};
    float reach;

    restLead.y += wave * 0.35f;
    restTrail.y += wave * 0.3f;

    switch (player->pose) {
    case PLAYER_POSE_LASER:
    case PLAYER_POSE_CRYO:
        /* Heat vision: clenched hands brace the chest; the eyes do the work. */
        *lead = (Vector2){2.1f, 1.3f + 4.5f * lean};
        *trail = (Vector2){-1.8f, 0.8f + 3.8f * lean};
        return;
    case PLAYER_POSE_CHILL:
        /* Both palms out: a wide, two-handed gesture, so the cryo beam does not
           look like the laser with a different colour, and so the telekinetic
           hold has two hands to leave from. PlayerHandOrigin builds the same
           two points in world space for whatever is cast from them. */
        reach = PLAYER_TWO_HAND_REACH;
        lead->x = aimLocal.x * reach + 1.1f;
        lead->y = SHOULDER_UP + aimLocal.y * reach - 0.6f;
        trail->x = aimLocal.x * (reach - 0.8f) - 0.8f;
        trail->y = SHOULDER_UP + aimLocal.y * (reach - 0.8f) - 2.2f;
        return;
    case PLAYER_POSE_PUSH: {
        /* Both palms flat on the rock, shoulder-width apart across the
           direction of the push. Short reach and no animation: he is leaning
           on it, not striking it, and the arms are what the weight goes
           through. */
        float brace = PLAYER_TWO_HAND_REACH - 1.0f;
        Vector2 across = {-pushLocal.y, pushLocal.x};

        lead->x = pushLocal.x * brace + across.x * 1.6f;
        lead->y = SHOULDER_UP + pushLocal.y * brace + across.y * 1.6f;
        trail->x = pushLocal.x * brace - across.x * 1.6f;
        trail->y = SHOULDER_UP + pushLocal.y * brace - across.y * 1.6f;
        return;
    }
    case PLAYER_POSE_BLAST: {
        /* Impact is immediate in gameplay. Start fully extended, hold briefly,
           then recover with the other fist guarding the ribs. */
        float elapsed = 1.0f - Clamp(player->poseTimer / 0.28f, 0.0f, 1.0f);
        float recover = Clamp((elapsed - 0.18f) / 0.82f, 0.0f, 1.0f);
        float thrust = 7.4f - 5.2f * recover * recover * (3.0f - 2.0f * recover);

        lead->x = aimLocal.x * thrust + 1.0f;
        lead->y = SHOULDER_UP + aimLocal.y * thrust - 0.4f;
        *trail = (Vector2){-1.4f, 1.7f};
        return;
    }
    default:
        break;
    }

    if (player->mode == PLAYER_MODE_WALK) {
        /* On foot the arms swing against the legs, wider at a run, and go
           up and out in a jump. */
        float moving = player->grounded
                           ? Clamp(fabsf(player->velocity.x) / 20.0f, 0.0f, 1.0f)
                           : 0.0f;
        float swing = sinf(player->walkPhase * 2.0f * PI) * moving *
                      (0.8f + 0.9f * Clamp((fabsf(player->velocity.x) -
                                            PLAYER_WALK_SPEED) /
                                               (PLAYER_RUN_SPEED - PLAYER_WALK_SPEED),
                                           0.0f, 1.0f));

        float run = Clamp((fabsf(player->velocity.x) - PLAYER_WALK_SPEED) /
                           (PLAYER_RUN_SPEED - PLAYER_WALK_SPEED), 0.0f, 1.0f);
        if (player->grounded && player->landingTimer > 0.0f) {
            float recovery = player->landingTimer / PLAYER_LANDING_RECOVERY;
            *lead = (Vector2){2.65f, -3.1f * recovery};
            *trail = (Vector2){-1.8f, 0.8f};
            return;
        }
        if (!player->grounded && player->velocity.y > PLAYER_HEAVY_LANDING_SPEED) {
            *lead = (Vector2){2.0f, 0.6f};
            *trail = (Vector2){-1.9f, 0.2f};
            return;
        }
        if (!player->grounded) {
            float rise = player->velocity.y < 0.0f ? 1.0f : 0.4f;

            restLead = (Vector2){2.6f, SHOULDER_UP + 0.6f + 1.4f * rise};
            restTrail = (Vector2){-2.4f, SHOULDER_UP - 0.4f + 1.0f * rise};
        } else {
            restLead = (Vector2){1.95f - swing, -1.15f + run * 1.95f + fabsf(swing) * 0.15f};
            restTrail = (Vector2){-1.8f + swing, -1.0f + run * 1.8f + fabsf(swing) * 0.15f};
        }
        *lead = restLead;
        *trail = restTrail;
        return;
    }

    /* Free flight: the leading arm still tracks the cursor, so aim stays
       readable, but only part of the way — the whole arm swinging to the cursor
       while hovering looks like pointing, not like flying. */
    {
        float speed = Vector2Length(player->velocity);
        float opposing = speed > 60.0f
                             ? -(player->thrust.x * player->velocity.x +
                                 player->thrust.y * player->velocity.y) / speed
                             : 0.0f;
        float brake = Clamp((opposing - 0.35f) / 0.65f, 0.0f, 1.0f);

        /* Pushing against his own velocity is a deliberate air-brake: both
           hands open in front and the trailing leg becomes a counterweight. */
        lead->x = Lerp(restLead.x + aimLocal.x * 0.3f * (1.0f - lean),
                       2.5f, brake);
        lead->y = Lerp(restLead.y + aimLocal.y * 0.3f * (1.0f - lean),
                       6.9f, brake);
        trail->x = Lerp(restTrail.x, -1.7f, brake);
        trail->y = Lerp(restTrail.y, 5.9f, brake);
    }
}

/* Every tone the figure is painted in. One struct rather than a dozen locals
   so the same figure can be drawn twice: in colour for the scene, and in
   solid black for the emissive plane, where it has to occlude whatever glows
   behind it. */
typedef struct PlayerPalette {
    Color dark;
    Color mid;
    Color lit;
    Color capeCore;
    Color capeEdge;
    Color capeShade;
    Color skin;
    Color limbDark;
    Color limbMid;
    Color trim;
    Color accent;
    Color glowLaser;
    Color glowChill;
    Color glowBlast;
    Color hair;
    Color silver;
    Color skinShadow;
    Color stubble;
} PlayerPalette;

static Color PlayerTint(Color from, Color toward, float amount)
{
    float t = Clamp(amount, 0.0f, 1.0f);

    return (Color){(unsigned char)Lerp((float)from.r, (float)toward.r, t),
                   (unsigned char)Lerp((float)from.g, (float)toward.g, t),
                   (unsigned char)Lerp((float)from.b, (float)toward.b, t),
                   from.a};
}

static PlayerPalette PlayerPaletteFor(const Player *player,
                                      PlayerVisualEnvironment environment)
{
    /* Dark-red suit, pale trim and weathered grey cape. The face and the
       broken-up planes of the chest keep the small figure legible. */
    PlayerPalette palette = {
        .dark = {76, 31, 40, 255},
        .mid = {145, 46, 49, 255},
        .lit = {194, 76, 65, 255},
        .capeCore = {115, 131, 134, 255},
        .capeEdge = {173, 187, 179, 255},
        .capeShade = {98, 105, 109, 255},
        .skin = {230, 180, 137, 255},
        /* The far-side limbs sit in a much darker tone than the torso. That
           separation, not an outline, is what puts them behind the body. Dark
           enough to sit behind the body, light enough to still be a limb: at
           the value of the background the far leg disappears and only its
           boot remains, reading as a square floating beside the character. */
        .limbDark = {68, 32, 41, 255},
        .limbMid = {129, 43, 48, 255},
        .trim = {166, 181, 176, 255},
        .accent = {226, 77, 53, 255},
        .hair = {36, 34, 37, 255},
        .silver = {111, 110, 103, 255},
        .skinShadow = {133, 104, 92, 255},
        .stubble = {82, 47, 39, 255},
        .glowLaser = {255, 224, 168, 235},
        .glowChill = {206, 244, 255, 235},
        .glowBlast = {196, 222, 255, 235},
    };

    if (environment.water > 0.0f) {
        float wet = Clamp(environment.water, 0.0f, 1.0f);

        palette.dark = PlayerTint(palette.dark, (Color){46, 56, 71, 255}, wet * 0.38f);
        palette.mid = PlayerTint(palette.mid, (Color){84, 91, 113, 255}, wet * 0.42f);
        palette.lit = PlayerTint(palette.lit, (Color){131, 151, 163, 255}, wet * 0.38f);
        palette.capeCore = PlayerTint(palette.capeCore, (Color){54, 99, 123, 255}, wet * 0.55f);
        palette.capeEdge = PlayerTint(palette.capeEdge, (Color){111, 164, 182, 255}, wet * 0.45f);
    }
    if (environment.heat > 0.0f) {
        float hot = Clamp(environment.heat, 0.0f, 1.0f);

        palette.lit = PlayerTint(palette.lit, (Color){252, 151, 90, 255}, hot * 0.6f);
        palette.trim = PlayerTint(palette.trim, (Color){255, 189, 118, 255}, hot * 0.75f);
        palette.capeEdge = PlayerTint(palette.capeEdge, (Color){211, 123, 93, 255}, hot * 0.4f);
    }
    if (player->impactTimer > 0.0f) {
        /* Flash the fills, not a rim: brightening the body sells the hit. */
        palette.dark = (Color){186, 154, 96, 255};
        palette.mid = (Color){245, 226, 168, 255};
        palette.lit = (Color){255, 252, 232, 255};
        palette.limbDark = (Color){170, 138, 84, 255};
        palette.limbMid = (Color){228, 202, 142, 255};
        palette.trim = (Color){255, 250, 226, 255};
        palette.accent = (Color){255, 255, 255, 255};
    }
    return palette;
}

static PlayerPalette PlayerPaletteSilhouette(void)
{
    const Color black = {0, 0, 0, 255};
    PlayerPalette palette;

    palette.dark = black;
    palette.mid = black;
    palette.lit = black;
    palette.capeCore = black;
    palette.capeEdge = black;
    palette.capeShade = black;
    palette.skin = black;
    palette.limbDark = black;
    palette.limbMid = black;
    palette.trim = black;
    palette.accent = black;
    palette.glowLaser = black;
    palette.glowChill = black;
    palette.glowBlast = black;
    palette.hair = black;
    palette.silver = black;
    palette.skinShadow = black;
    palette.stubble = black;
    return palette;
}

/* The figure itself. `silhouette` draws only what has a body — cape, limbs,
   torso, head — and none of the exhaust, streaks and sparks around it, which
   are light rather than matter and must not occlude anything. */
static void PlayerRendererDrawFigure(const Player *player, Vector2 aimPosition,
                                     const PlayerPalette *palette,
                                     PlayerVisualEnvironment environment,
                                     bool silhouette)
{
    const Color capeCore = palette->capeCore;
    const Color capeEdge = palette->capeEdge;
    const Color capeShade = palette->capeShade;
    const Color skin = palette->skin;
    const Color limbDark = palette->limbDark;
    const Color limbMid = palette->limbMid;
    const Color trim = palette->trim;
    const Color accent = palette->accent;
    const Color lit = palette->lit;
    const Color mid = palette->mid;
    const Color dark = palette->dark;
    BodyFrame frame;
    Vector2 aimLocal;
    Vector2 pushLocal;
    Vector2 travel = {0.0f, -1.0f};
    Vector2 leadHand;
    Vector2 trailHand;
    Vector2 shoulderLead;
    Vector2 shoulderTrail;
    Vector2 hipLead;
    Vector2 hipTrail;
    Vector2 kneeLead;
    Vector2 kneeTrail;
    float aimX;
    float aimY;
    float aimLength;
    float speed;
    float lean;
    float wave;
    float bob;
    float kneeDrop;
    float footBack;
    Vector2 back;
    float sideSign;
    int segment;

    if (player == NULL) {
        return;
    }

    speed = sqrtf(player->velocity.x * player->velocity.x +
                  player->velocity.y * player->velocity.y);
    lean = Clamp(player->leanAmount, 0.0f, 1.0f);
    if (speed > 0.001f) {
        travel = (Vector2){player->velocity.x / speed, player->velocity.y / speed};
    }

    /* The body axis turns from straight up toward the direction of travel. `up`
       runs from the hips to the head, so at full lean it *is* the direction of
       travel: the head leads and the feet trail.

       Taken from the player module rather than derived again here. The beam
       origin needs the same axis, and two derivations of where the head is are
       two heads — which is exactly how the laser ended up leaving from the
       chest while the visor was drawn somewhere else. */
    frame.up = PlayerBodyUp(player);
    frame.side = (Vector2){-frame.up.y, frame.up.x};

    aimX = aimPosition.x - player->position.x;
    aimY = aimPosition.y - player->position.y;
    aimLength = sqrtf(aimX * aimX + aimY * aimY);
    if (aimLength > 0.001f) {
        aimX /= aimLength;
        aimY /= aimLength;
    } else {
        aimX = player->facingRight ? 1.0f : -1.0f;
        aimY = 0.0f;
    }
    /* The cursor expressed in the body frame, so arm poses can be written once
       and follow the body however it is turned. */
    aimLocal = (Vector2){aimX * frame.side.x + aimY * frame.side.y,
                         aimX * frame.up.x + aimY * frame.up.y};

    /* Which way the shoulders face. Aim decides it while hovering; at speed the
       body follows the travel, or the character would fly sideways. */
    sideSign = aimLocal.x >= 0.0f ? 1.0f : -1.0f;
    frame.side.x *= sideSign;
    frame.side.y *= sideSign;
    aimLocal.x *= sideSign;
    /* The push expressed in the same frame. Taken from the thrust rather than
       from the cursor: the hands go where the character is leaning, and the
       cursor is free to be somewhere else entirely. */
    pushLocal = (Vector2){player->thrust.x * frame.side.x +
                              player->thrust.y * frame.side.y,
                          player->thrust.x * frame.up.x +
                              player->thrust.y * frame.up.y};

    /* Which way is "behind the character". Standing still there is no direction
       of travel to use, and taking one anyway tucks the feet upward; behind is
       then simply the far side of the shoulders. */
    back.x = -frame.side.x * (1.0f - lean) - travel.x * lean;
    back.y = -frame.side.y * (1.0f - lean) - travel.y * lean;
    {
        float length = sqrtf(back.x * back.x + back.y * back.y);

        if (length < 0.001f) {
            back = (Vector2){-frame.side.x, -frame.side.y};
        } else {
            back.x /= length;
            back.y /= length;
        }
    }

    wave = sinf(player->animationTime * (player->boosting ? 15.0f : 5.0f));
    frame.origin = PlayerBodyOrigin(player);
    bob = Vector2DotProduct(Vector2Subtract(frame.origin, player->position), frame.up);

    /* ---- acceleration burst, behind the body ---- */
    if (player->boostBurstTimer > 0.0f && !silhouette) {
        float progress = 1.0f - player->boostBurstTimer / PLAYER_BOOST_BURST_TIME;
        float radius = BODY(3.0f + progress * 18.0f);
        float alpha = (1.0f - progress) * 0.8f;
        Color ring = (Color){137, 224, 255, 255};
        int ringIndex;

        for (ringIndex = 0; ringIndex < 2; ++ringIndex) {
            float ringRadius = radius - BODY((float)ringIndex * 3.0f);

            if (ringRadius > 1.0f) {
                /* The block never shrinks below a cell: a ring drawn out of
                   half-cells is a ring nobody can see. */
                PlayerFxRing(player->position, ringRadius,
                             fmaxf(1.0f, BODY(1.5f)),
                             Fade(ring,
                                  alpha / (1.0f + (float)ringIndex * 0.35f)));
            }
        }
    }

    /* ---- cape ---- */
    {
        /* The cape streams opposite the travel and sags when hovering, so it
           says which way the character is moving before the body does.
           Attached at the far shoulder, not floating beside the figure. */
        Vector2 anchor = BodyPoint(&frame, 3.6f, -0.9f);
        float impact = player->landingTimer > 0.0f
                           ? player->landingTimer / PLAYER_LANDING_RECOVERY
                           : 0.0f;
        Vector2 flow = {back.x * (0.3f + 0.7f * lean) -
                            frame.up.x * (1.0f - lean) * (0.95f - impact * 0.35f),
                        back.y * (0.3f + 0.7f * lean) -
                            frame.up.y * (1.0f - lean) * (0.95f - impact * 0.35f)};
        float flowLength = sqrtf(flow.x * flow.x + flow.y * flow.y);
        float length = BODY(7.2f + 2.5f * lean);
        int steps = 64;

        if (flowLength > 0.001f) {
            flow.x /= flowLength;
            flow.y /= flowLength;
        }
        for (segment = 0; segment <= steps; ++segment) {
            float amount = (float)segment / (float)steps;
            float along = length * amount;
            /* Barely a stir while hovering — cloth hangs — and a real wave at
               speed. */
            float airflow = Clamp(speed / player->maxSpeed, 0.0f, 1.0f);
            float ripple = (sinf(player->animationTime * (4.0f + airflow * 7.0f) -
                                  amount * 5.4f) +
                            0.25f * sinf(player->animationTime * 13.0f - amount * 11.0f)) *
                           BODY(amount * amount * (0.25f + airflow * 1.3f + impact * 0.8f));
            Vector2 spine = {anchor.x + flow.x * along - flow.y * ripple,
                             anchor.y + flow.y * along + flow.x * ripple};
            float width = BODY((1.05f + 1.15f * amount) * (1.0f - 0.35f * lean));
            float across;

            for (across = -width; across <= width + 0.001f;
                 across += BODY(0.2f)) {
                /* Two weighted points at the hem, with a shallow cleft. */
                if (amount > 0.83f && fabsf(across) <
                    BODY((amount - 0.83f) * 2.3f)) continue;
                Color tone = across < -width * 0.45f ||
                                     sinf(across / BODY(1.15f) + amount * 2.0f) < -0.72f
                                 ? capeShade
                                 : (across > width * 0.45f ? capeEdge : capeCore);

                DrawBodyCell((Vector2){spine.x - flow.y * across,
                                       spine.y + flow.x * across},
                             1, tone);
            }
        }
    }

    /* ---- legs ---- */
    if (player->mode == PLAYER_MODE_WALK) {
        /* The soles on the bottom of the collider, wherever the bob has put
           the hips. */
        float ground = (-PlayerExtent(player) - bob) / PLAYER_BODY_SCALE + 0.2f;
        Vector2 footLead;
        Vector2 footTrail;
        Vector2 hips[2] = {{0.6f, -0.6f}, {-0.6f, -0.6f}};
        Vector2 knees[2];

        PlayerWalkFeet(player, frame.side.x >= 0.0f ? 1.0f : -1.0f, ground,
                       &footLead, &footTrail);
        knees[0] = PlayerKnee(hips[0], footLead);
        knees[1] = PlayerKnee(hips[1], footTrail);
        if (player->grounded && player->landingTimer <= 0.0f) {
            float moving = Clamp(fabsf(player->velocity.x) / 20.0f, 0.0f, 1.0f);
            Vector2 straightLead = Vector2Lerp(hips[0], footLead, 0.5f);
            Vector2 straightTrail = Vector2Lerp(hips[1], footTrail, 0.5f);
            straightLead.x += 0.12f;
            straightTrail.x += 0.12f;
            knees[0] = Vector2Lerp(straightLead, knees[0], moving);
            knees[1] = Vector2Lerp(straightTrail, knees[1], moving);
        }
        hipLead = BodyPoint(&frame, hips[0].y, hips[0].x);
        hipTrail = BodyPoint(&frame, hips[1].y, hips[1].x);
        kneeLead = BodyPoint(&frame, knees[0].y, knees[0].x);
        kneeTrail = BodyPoint(&frame, knees[1].y, knees[1].x);
        PlayerDrawLeg(&frame, hipTrail, kneeTrail,
                      BodyPoint(&frame, footTrail.y, footTrail.x), limbDark,
                      dark, dark);
        PlayerDrawLeg(&frame, hipLead, kneeLead,
                      BodyPoint(&frame, footLead.y, footLead.x), limbMid,
                      mid, trim);
    } else {
        /* Knees stay drawn back while hovering and straighten out as the body
           lays down. */
        Vector2 footLead;
        Vector2 footTrail;

        kneeDrop = 3.4f - 0.9f * lean;
        footBack = (1.3f - 1.0f * lean) + wave * 0.35f * (1.0f - lean);
        /* A wide enough stance that the two legs stay two legs. */
        hipLead = BodyPoint(&frame, -0.6f, 0.6f);
        hipTrail = BodyPoint(&frame, -0.6f, -0.6f);
        kneeLead = BodyPoint(&frame, -0.6f - kneeDrop, 1.1f + 0.3f * (1.0f - lean));
        kneeTrail = BodyPoint(&frame, -0.6f - kneeDrop, -1.1f - 0.3f * (1.0f - lean));
        footLead = (Vector2){
            kneeLead.x - frame.up.x * BODY(2.6f) + back.x * BODY(footBack),
            kneeLead.y - frame.up.y * BODY(2.6f) + back.y * BODY(footBack)};
        footTrail = (Vector2){
            kneeTrail.x - frame.up.x * BODY(2.6f) + back.x * BODY(footBack),
            kneeTrail.y - frame.up.y * BODY(2.6f) + back.y * BODY(footBack)};
        PlayerDrawLeg(&frame, hipTrail, kneeTrail, footTrail, limbDark,
                      dark, dark);
        PlayerDrawLeg(&frame, hipLead, kneeLead, footLead, limbMid,
                      mid, trim);
    }

    /* ---- arms ---- */
    shoulderLead = BodyPoint(&frame, SHOULDER_UP, 1.8f);
    shoulderTrail = BodyPoint(&frame, SHOULDER_UP, -1.8f);
    PlayerHandTargets(player, aimLocal, pushLocal, lean, wave, &leadHand,
                      &trailHand);
    {
        Vector2 leadPoint = BodyPoint(&frame, leadHand.y, leadHand.x);
        Vector2 trailPoint = BodyPoint(&frame, trailHand.y, trailHand.x);
        if (player->pose == PLAYER_POSE_CHILL) {
            leadPoint = PlayerHandOrigin(player, aimPosition, false);
            trailPoint = PlayerHandOrigin(player, aimPosition, true);
        }
        Vector2 leadElbow = {(shoulderLead.x + leadPoint.x) * 0.5f +
                                 frame.side.x * BODY(0.6f),
                             (shoulderLead.y + leadPoint.y) * 0.5f +
                                 frame.side.y * BODY(0.6f)};
        Vector2 trailElbow = {(shoulderTrail.x + trailPoint.x) * 0.5f -
                                  frame.side.x * BODY(0.6f),
                              (shoulderTrail.y + trailPoint.y) * 0.5f -
                                  frame.side.y * BODY(0.6f)};

        if (player->pose == PLAYER_POSE_BLAST) {
            Vector2 acrossAim = {-aimY, aimX};

            leadElbow.x -= acrossAim.x * BODY(1.1f);
            leadElbow.y -= acrossAim.y * BODY(1.1f);
        }
        DrawLimb(shoulderTrail, trailElbow, 3, dark);
        DrawLimb(trailElbow, trailPoint, 2, limbDark);
        DrawBodyCell(trailPoint, 3, limbDark);

        DrawLimb(shoulderLead, leadElbow, 3, lit);
        DrawLimb(leadElbow, leadPoint, 2, limbMid);
        DrawBodyCell(leadElbow, 2, mid);
        DrawBodyCell(leadPoint, 3, trim);

        if (player->pose == PLAYER_POSE_CHILL) {
            DrawBodyCell(leadPoint, 2, palette->glowChill);
            DrawBodyCell(trailPoint, 2, palette->glowChill);
        }
    }

    /* ---- torso, neck, head ---- */
    FillBodyRect(&frame, -0.5f, 0.75f, 0.92f, dark, mid, lit);
    FillBodyRect(&frame, 0.8f, 1.55f, 1.13f, dark, mid, lit);
    FillBodyRect(&frame, 1.6f, 3.1f, 1.50f, dark, mid, lit);
    /* Shoulder plates, a shade proud of the chest: the suit has a
       structure, and the arms have somewhere to hang from. */
    FillBodyRect(&frame, 3.0f, 3.48f, 1.75f, dark, mid, lit);
    /* A belt breaks the torso into a chest and a waist. */
    FillBodyRect(&frame, -0.38f, -0.12f, 1.05f, capeShade, capeCore, capeShade);
    /* Offset seams form Emberfall's own small mark rather than a borrowed
       superhero crest. */
    DrawLimb(BodyPoint(&frame, 3.15f, -0.72f), BodyPoint(&frame, 1.72f, 0.18f),
             1, trim);
    DrawLimb(BodyPoint(&frame, 1.66f, 0.2f), BodyPoint(&frame, 2.15f, 0.55f),
             1, accent);
    DrawLimb(BodyPoint(&frame, 0.35f, -0.55f), BodyPoint(&frame, 1.34f, -0.68f),
             1, dark);

    /* Neck stays visible between the face and shoulder line. */
    FillBodyRect(&frame, 3.55f, 3.95f, 0.77f, capeShade, dark, mid);
    FillBodyRect(&frame, 4.0f, 4.68f, 0.46f, palette->skinShadow, skin, skin);

    /* Bare face, short swept hair, weathered temple and shaded stubble. */
    FillBodyRect(&frame, 4.72f, 4.95f, 0.63f, palette->skinShadow, skin, skin);
    FillBodyRect(&frame, 4.97f, 6.40f, 0.82f, palette->skinShadow, skin, skin);
    FillBodyRect(&frame, 6.43f, 6.86f, 0.85f,
                 palette->hair, palette->hair, palette->hair);
    DrawBodyCell(BodyPoint(&frame, 6.16f, -0.80f), 1, palette->silver);
    DrawBodyCell(BodyPoint(&frame, 6.54f, -0.71f), 1, palette->silver);
    DrawLimb(BodyPoint(&frame, 4.88f, -0.35f), BodyPoint(&frame, 5.13f, 0.54f),
             1, palette->stubble);
    DrawBodyCell(BodyPoint(&frame, 5.66f, 1.03f), 1, skin);
    DrawLimb(BodyPoint(&frame, 6.18f, 0.38f), BodyPoint(&frame, 6.18f, 1.0f),
             1, palette->hair);
    {
        Vector2 eye = PlayerVisorOrigin(player, aimPosition);
        Color eyeColor = player->pose == PLAYER_POSE_LASER ? palette->glowLaser :
                         (player->pose == PLAYER_POSE_CRYO ||
                          player->pose == PLAYER_POSE_CHILL ? palette->glowChill :
                          palette->hair);
        DrawBodyCell(eye, 1, eyeColor);
    }

    if (silhouette) {
        return;
    }

    if (environment.water > 0.1f) {
        unsigned char alpha = (unsigned char)(145.0f * Clamp(environment.water, 0.0f, 1.0f));
        Color sheen = {176, 215, 221, alpha};

        DrawBodyCell(BodyPoint(&frame, 3.25f, 1.7f), 1, sheen);
        DrawBodyCell(BodyPoint(&frame, 1.85f, 1.42f), 1, sheen);
        DrawBodyCell(BodyPoint(&frame, -1.25f, 1.0f), 1, sheen);
    }

    /* ---- drill contact ---- */
    if (player->drilledCells > 0 && speed > 0.001f) {
        Vector2 across = {-travel.y, travel.x};
        int spark;

        for (spark = -1; spark <= 1; ++spark) {
            Vector2 point = {
                player->drillPosition.x + across.x * (float)spark * BODY(2.5f),
                player->drillPosition.y + across.y * (float)spark * BODY(2.5f)};

            DrawBodyCell(point, spark == 0 ? 3 : 1,
                         spark == 0 ? (Color){255, 214, 96, 245} : accent);
        }
    }
}

void PlayerRendererDraw(const Player *player, Vector2 aimPosition,
                        PlayerVisualEnvironment environment)
{
    PlayerPalette palette;

    if (player == NULL) {
        return;
    }
    palette = PlayerPaletteFor(player, environment);
    PlayerRendererDrawFigure(player, aimPosition, &palette, environment, false);
}

void PlayerRendererDrawSilhouette(const Player *player, Vector2 aimPosition)
{
    PlayerPalette palette;

    if (player == NULL) {
        return;
    }
    palette = PlayerPaletteSilhouette();
    PlayerRendererDrawFigure(player, aimPosition, &palette,
                             (PlayerVisualEnvironment){0}, true);
}

void PlayerRendererDrawEmissive(const Player *player)
{
    if (player == NULL) {
        return;
    }
    if (player->boosting) {
        Vector2 direction = Vector2Normalize(player->velocity);
        Vector2 glow = Vector2Add(player->position, Vector2Scale(direction, BODY(-4.0f)));
        PlayerFxBlob(glow, BODY(1.6f), 1.0f, (Color){104, 166, 183, 65});
    }
    if (player->boostBurstTimer > 0.0f) {
        float progress = 1.0f - player->boostBurstTimer / PLAYER_BOOST_BURST_TIME;
        float radius = BODY(3.0f + progress * 18.0f);

        PlayerFxRing(player->position, radius, fmaxf(1.0f, BODY(1.5f)),
                     Fade((Color){150, 224, 255, 255},
                          (1.0f - progress) * 0.75f));
    }
    if (player->drilledCells > 0) {
        PlayerFxBlob(player->drillPosition, BODY(2.4f), 1.0f,
                     (Color){255, 151, 42, 215});
    }
}
