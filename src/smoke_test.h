#ifndef SMOKE_TEST_H
#define SMOKE_TEST_H

/* The scripted GL session behind `--smoke-test`.
 *
 * One deterministic run that exercises the renderer and the gameplay together
 * — beams, blasts, bodies, the day turning, space, a full flight — and takes
 * the reference screenshots. It steps exactly one fixed tick per frame, so a
 * frame is a known amount of simulated time and every assertion about where
 * something got to is repeatable.
 *
 * It lives beside main.c rather than inside it so the composition root stays
 * a composition root. The frame loop calls it in phases, in the order the
 * phases are declared below, and each phase says what it touches; the run
 * never reaches into the loop's own state except through these calls.
 */

#include <stdbool.h>

#include <raylib.h>

#include "camera_feedback.h"
#include "game.h"
#include "game_events.h"
#include "input.h"
#include "renderer.h"
#include "terrain_extraction.h"

typedef struct SmokeAcceptance {
    Vector2 platformAt;
    Vector2 supportAt;
    bool detached;
    bool pushed;
    bool grabbed;
    bool dragged;
    bool threw;
    bool carved;
    bool split;
    /* Presentation observed while the gameplay phase was running. The two
       features were built on separate branches, so "they both work" is not the
       same claim as "they work at the same time", and only the second is worth
       asserting. */
    bool fxDuringPlay;
    bool cameraFeedbackDuringPlay;
    float pushSpeed;
    float dragDistance;
    float throwSpeed;
    int fragments;
    Vector2 dragStart;
} SmokeAcceptance;

typedef struct SmokeMovement {
    Vector2 origin;
    float cruiseSpeed;
    float boostSpeed;
    float peakSpeed;
    float turnLateral;
    float brakeFrom;
    float reverseSpeed;
    float finalSpeed;
    /* Speed on the frame the wall goes up, and the slowest the flight ever got
       while it was inside it. Rock no longer costs the boost anything, and the
       only honest way to say so is to measure it going in and coming out. */
    float drillEntrySpeed;
    float drillLowSpeed;
    int drilled;
    int brakeFrames;
    bool turned;
    bool reversed;
    bool stopped;
    /* On foot: the fastest run along the floor, how many frames of it were
       on the ground, how high the jump rose, and whether the second jump
       took off into flight. */
    float walkFloorY;
    float runSpeed;
    float jumpRise;
    int groundedFrames;
    bool tookOff;
} SmokeMovement;

typedef struct SmokeTest {
    int frame;
    Vector2 aim;
    bool reactionObserved;
    bool laserHitObserved;
    bool explosionObserved;
    bool forceObserved;
    bool cryoObserved;
    bool boostObserved;
    bool collisionObserved;
    bool drillObserved;
    bool fireContained;
    bool resizeObserved;
    bool resizeRestored;
    bool bloomObserved;
    bool bloomResized;
    bool bloomRestored;
    bool targetsSynchronized;
    bool presentationFxObserved;
    bool environmentObserved;
    bool environmentViewValid;
    bool environmentCameraFeedback;
    bool environmentZoomOut;
    bool lightingObserved;
    uint8_t environmentPaletteMask;
    uint16_t environmentMaximumSceneDrawCalls;
    uint16_t environmentMaximumEmissiveDrawCalls;
    TerrainBodyHandle terrainBody;
    Vector2 terrainStartPosition;
    float terrainStartAngle;
    bool terrainExtracted;
    bool terrainWorldCleared;
    bool terrainRendered;
    bool terrainMoved;
    bool terrainRotated;
    bool terrainCollisionObserved;
    bool terrainCacheReleased;
    /* The automatic-detach acceptance run: a blast severs a pillar and the
       fixed step is expected to turn the block above it into a body without
       anything here reaching for the extraction API. */
    Vector2 detachBlast;
    bool autoDetachObserved;
    bool autoDetachEvent;
    /* The impulse half of the acceptance run: the blast has to throw what it
       freed, the light block has to outrun the heavy one, and a force blow has
       to move a body that is already lying there. */
    float lightSpeed;
    float heavySpeed;
    float thrownSpin;
    float forceSpeedBefore;
    float forceSpeedAfter;
    float forceStartX;
    float forceShiftX;
    bool massMattered;
    bool forceMovedBody;
    SmokeMovement movement;
    SmokeAcceptance acceptance;
    uint32_t terrainTextureUpdates;
    /* Bodies the render phase alone detached, so the upload count can be
       checked against what that phase produced rather than against everything
       the gameplay phase goes on to break apart. */
    int renderDetaches;
    uint32_t terrainMaximumDrawCalls;
    uint64_t terrainMaximumTextureBytes;
    double bloomSubmissionTotal;
    double bloomSubmissionMaximum;
    /* What the world costs to keep on screen, measured over the flight rather
       than read off one frame of the HUD. A single frame's number varies by
       several times between runs on a loaded machine, which is enough to tune
       a renderer in the wrong direction. */
    Vector2 groundPosition;
    Vector2 groundVelocity;
    double prepareTotal;
    double prepareMaximum;
    double lightTotal;
    double lightMaximum;
    int prepareFrames;
    /* The busiest the scheduler got. Checked instead of the count at the end
       of the run, because a world that has been left alone is supposed to have
       gone entirely to sleep by then. */
    int mostActiveChunks;
    int bloomFrames;
} SmokeTest;

/* Lays out every fixture in the freshly generated world and runs the headless
   probes. After GameInit, before the first frame. */
void SmokeTestPrepare(SmokeTest *smoke, GameState *game);
/* Everything the script does to the session before the frame's input is
   polled: window resizes, the widened view, palettes, the time of day, bodies
   freed and blasts fired on schedule. */
void SmokeTestBeginFrame(SmokeTest *smoke, GameState *game, Renderer *renderer,
                         CameraFeedback *feedback);
/* Replaces the polled input with the frame's scripted one. */
void SmokeTestScriptInput(SmokeTest *smoke, GameState *game, AppInput *input,
                          Vector2 *aimPosition, Vector2 *cursorCell);
/* After GameUpdate and the presentation consumers. */
void SmokeTestObserveUpdate(SmokeTest *smoke, const GameState *game,
                            const GameEventBuffer *events);
void SmokeTestObserveCamera(SmokeTest *smoke, CameraFeedbackOutput output);
/* After the scene has been rendered, before it is composited. */
void SmokeTestObserveRender(SmokeTest *smoke, const GameState *game,
                            const Renderer *renderer, Camera2D presentationCamera,
                            Camera2D aimCamera, CameraFeedbackOutput output);
/* Frames whose photograph must not have the debug panel over it. */
bool SmokeTestHidesHud(const SmokeTest *smoke);
/* Takes this frame's screenshots. Call inside the frame, after everything has
   been drawn and before EndDrawing: the back buffer after a swap is not this
   frame, it is whatever the driver left there. */
void SmokeTestCapture(SmokeTest *smoke);
/* Advances the script. True when the run is over and the loop should end. */
bool SmokeTestAdvance(SmokeTest *smoke);
/* Prints the measurements and the verdict. Returns the process exit code. */
int SmokeTestReport(const SmokeTest *smoke, const GameState *game,
                    const Renderer *renderer);

#endif
