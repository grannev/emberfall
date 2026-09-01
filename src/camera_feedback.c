#include "camera_feedback.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <raymath.h>

#define CAMERA_LOOKAHEAD_TIME 0.42f
#define CAMERA_LOOKAHEAD_VIEW_FRACTION 0.30f
#define CAMERA_FAST_VIEW_SCALE 2.0f
#define CAMERA_MAX_POSITION_IMPULSE 8.0f
#define CAMERA_MAX_ROTATION_DEGREES 1.10f
#define CAMERA_MAX_ZOOM_KICK 0.10f

static float CameraFeedbackClamp(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static Vector2 CameraFeedbackDirection(Vector2 value, Vector2 fallback)
{
    float length = Vector2Length(value);

    return length > 0.001f ? Vector2Scale(value, 1.0f / length) : fallback;
}

static void CameraFeedbackCountDrop(CameraFeedback *feedback)
{
    if (feedback->stats.dropped < UINT32_MAX) {
        ++feedback->stats.dropped;
    }
}

static float CameraImpulseWeight(const CameraImpulse *impulse)
{
    return impulse->positionStrength + impulse->rotationStrength * 2.0f +
           impulse->zoomStrength * 40.0f;
}

/* The stack is intentionally tiny. When full, a stronger impact may replace
   the weakest one; an inconsequential scrape may not erase an explosion. */
static bool CameraFeedbackPush(CameraFeedback *feedback,
                               CameraImpulse impulse)
{
    uint8_t slot;

    if (feedback->stats.active < CAMERA_IMPULSE_CAPACITY) {
        slot = feedback->stats.active++;
        if (feedback->stats.active > feedback->stats.peak) {
            feedback->stats.peak = feedback->stats.active;
        }
    } else {
        float incomingWeight = CameraImpulseWeight(&impulse);
        float weakestWeight = CameraImpulseWeight(&feedback->impulses[0]);
        uint8_t index;

        slot = 0u;
        for (index = 1u; index < feedback->stats.active; ++index) {
            float weight = CameraImpulseWeight(&feedback->impulses[index]);

            if (weight < weakestWeight) {
                weakestWeight = weight;
                slot = index;
            }
        }
        CameraFeedbackCountDrop(feedback);
        if (incomingWeight < weakestWeight) {
            return false;
        }
    }
    feedback->impulses[slot] = impulse;
    return true;
}

static float CameraFeedbackPhase(const GameEvent *event, uint16_t index)
{
    float value = event->position.x * 0.173f + event->position.y * 0.317f +
                  (float)event->type * 0.73f + (float)index * 0.41f;

    return fmodf(fabsf(value), 2.0f * PI);
}

void CameraFeedbackInit(CameraFeedback *feedback)
{
    if (feedback == NULL) {
        return;
    }
    memset(feedback, 0, sizeof(*feedback));
    feedback->viewScale = 1.0f;
}

void CameraFeedbackClear(CameraFeedback *feedback)
{
    CameraFeedbackInit(feedback);
}

void CameraFeedbackConsumeEvents(CameraFeedback *feedback,
                                 const GameEventBuffer *events,
                                 Vector2 playerPosition)
{
    uint16_t index;

    if (feedback == NULL || events == NULL) {
        return;
    }
    for (index = 0u; index < events->count; ++index) {
        const GameEvent *event = &events->events[index];
        CameraImpulse impulse = {0};
        bool relevant = true;

        impulse.phase = CameraFeedbackPhase(event, index);
        switch (event->type) {
        case GAME_EVENT_EXPLOSION:
            impulse.direction = CameraFeedbackDirection(
                Vector2Subtract(playerPosition, event->position),
                (Vector2){1.0f, 0.0f});
            impulse.positionStrength = CameraFeedbackClamp(
                event->radius * 0.11f, 3.0f, 6.4f);
            impulse.rotationStrength = 0.72f;
            impulse.zoomStrength = 0.060f;
            impulse.duration = 0.42f;
            break;
        case GAME_EVENT_FORCE:
            /* The punch no longer moves the player, so what sells its weight is
               the camera. A short, hard shove along the blow, not away from
               it: the frame lurches after the strike. */
            impulse.direction = CameraFeedbackDirection(event->direction,
                                                        (Vector2){1.0f, 0.0f});
            impulse.positionStrength = 7.4f;
            impulse.rotationStrength = 0.95f;
            impulse.zoomStrength = 0.072f;
            impulse.duration = 0.34f;
            break;
        case GAME_EVENT_BOOST_STAGE: {
            int stage = event->count < 1 ? 1 :
                        (event->count > 3 ? 3 : event->count);

            impulse.direction = Vector2Negate(CameraFeedbackDirection(
                event->direction, (Vector2){1.0f, 0.0f}));
            impulse.positionStrength = stage == 1 ? 0.75f :
                                       (stage == 2 ? 1.5f : 2.7f);
            impulse.rotationStrength = stage == 1 ? 0.10f :
                                       (stage == 2 ? 0.22f : 0.40f);
            impulse.zoomStrength = stage == 1 ? 0.010f :
                                   (stage == 2 ? 0.026f : 0.050f);
            impulse.duration = stage == 3 ? 0.34f : 0.22f;
            break;
        }
        case GAME_EVENT_PLAYER_IMPACT:
            if (event->strength < 14.0f) {
                relevant = false;
                break;
            }
            impulse.direction = CameraFeedbackDirection(
                event->direction, (Vector2){0.0f, -1.0f});
            impulse.positionStrength = CameraFeedbackClamp(
                (event->strength - 10.0f) * 0.026f, 0.25f, 2.8f);
            impulse.rotationStrength = CameraFeedbackClamp(
                event->strength * 0.003f, 0.08f, 0.42f);
            impulse.zoomStrength = CameraFeedbackClamp(
                event->strength * 0.00018f, 0.0f, 0.026f);
            impulse.duration = 0.22f;
            break;
        case GAME_EVENT_PLAYER_DRILL:
            impulse.direction = Vector2Negate(CameraFeedbackDirection(
                event->direction, (Vector2){1.0f, 0.0f}));
            impulse.positionStrength = CameraFeedbackClamp(
                0.22f + (float)event->count * 0.024f, 0.22f, 1.10f);
            impulse.rotationStrength = 0.10f;
            impulse.zoomStrength = 0.0f;
            impulse.duration = 0.12f;
            break;
        default:
            relevant = false;
            break;
        }
        if (relevant) {
            (void)CameraFeedbackPush(feedback, impulse);
        }
    }
}

static Vector2 CameraFeedbackTargetLookahead(CameraFeedbackMotion motion)
{
    float limitX = motion.viewWidth * CAMERA_LOOKAHEAD_VIEW_FRACTION;
    float limitY = motion.viewHeight * CAMERA_LOOKAHEAD_VIEW_FRACTION;

    return (Vector2){
        CameraFeedbackClamp(motion.velocity.x * CAMERA_LOOKAHEAD_TIME,
                            -limitX, limitX),
        CameraFeedbackClamp(motion.velocity.y * CAMERA_LOOKAHEAD_TIME,
                            -limitY, limitY),
    };
}

static float CameraFeedbackTargetViewScale(CameraFeedbackMotion motion)
{
    float speed = Vector2Length(motion.velocity);
    float range = motion.maximumSpeed - motion.normalSpeed;
    float excess;

    if (range <= 0.001f) {
        return 1.0f;
    }
    excess = CameraFeedbackClamp((speed - motion.normalSpeed) / range,
                                 0.0f, 1.0f);
    return 1.0f + (CAMERA_FAST_VIEW_SCALE - 1.0f) * excess;
}

CameraFeedbackOutput CameraFeedbackUpdate(CameraFeedback *feedback,
                                          CameraFeedbackMotion motion,
                                          float deltaTime)
{
    CameraFeedbackOutput output = {0};
    Vector2 targetLookahead;
    bool reversing;
    uint8_t index = 0u;

    if (feedback == NULL) {
        output.viewScale = 1.0f;
        return output;
    }

    /* A paused or invalid presentation frame must preserve the stable camera
       state. Returning the zero-initialized view scale would collapse the
       downstream zoom calculation even though no camera time elapsed. */
    output.lookahead = feedback->lookahead;
    output.viewScale = isfinite(feedback->viewScale) &&
                               feedback->viewScale >= 1.0f
                           ? feedback->viewScale
                           : 1.0f;
    if (!isfinite(deltaTime) || deltaTime <= 0.0f) {
        return output;
    }

    targetLookahead = CameraFeedbackTargetLookahead(motion);
    reversing = Vector2DotProduct(feedback->lookahead, targetLookahead) < 0.0f &&
                Vector2LengthSqr(feedback->lookahead) > 1.0f;
    /* On reversal the old lead first collapses toward the player. The camera
       therefore crosses no more than one focus point instead of whipping from
       one side of the view to the other in a single response curve. */
    if (reversing) {
        targetLookahead = (Vector2){0.0f, 0.0f};
    }
    feedback->lookahead = Vector2Lerp(
        feedback->lookahead, targetLookahead,
        1.0f - expf(-(reversing ? 7.0f : 5.5f) * deltaTime));
    feedback->viewScale +=
        (CameraFeedbackTargetViewScale(motion) - feedback->viewScale) *
        (1.0f - expf(-4.2f * deltaTime));

    while (index < feedback->stats.active) {
        CameraImpulse *impulse = &feedback->impulses[index];
        float progress = CameraFeedbackClamp(impulse->age / impulse->duration,
                                             0.0f, 1.0f);
        float remaining = 1.0f - progress;
        float envelope = remaining * remaining;
        /* The first kick always follows the event direction. Phase only
           decorrelates the small lateral/rotational response. */
        float primary = cosf(progress * 4.5f * PI) * envelope;
        float secondary = sinf(progress * 3.0f * PI + impulse->phase) * envelope;
        Vector2 normal = {-impulse->direction.y, impulse->direction.x};

        output.impulseOffset = Vector2Add(
            output.impulseOffset,
            Vector2Add(Vector2Scale(impulse->direction,
                                    impulse->positionStrength * primary),
                       Vector2Scale(normal,
                                    impulse->positionStrength * 0.18f * secondary)));
        output.rotationDegrees += impulse->rotationStrength * secondary;
        output.zoomKick += impulse->zoomStrength * envelope;

        impulse->age += deltaTime;
        if (impulse->age >= impulse->duration) {
            --feedback->stats.active;
            feedback->impulses[index] =
                feedback->impulses[feedback->stats.active];
            feedback->impulses[feedback->stats.active] = (CameraImpulse){0};
            continue;
        }
        ++index;
    }
    if (Vector2LengthSqr(output.impulseOffset) >
        CAMERA_MAX_POSITION_IMPULSE * CAMERA_MAX_POSITION_IMPULSE) {
        output.impulseOffset = Vector2Scale(
            Vector2Normalize(output.impulseOffset), CAMERA_MAX_POSITION_IMPULSE);
    }
    output.lookahead = feedback->lookahead;
    output.viewScale = feedback->viewScale;
    output.rotationDegrees = CameraFeedbackClamp(
        output.rotationDegrees, -CAMERA_MAX_ROTATION_DEGREES,
        CAMERA_MAX_ROTATION_DEGREES);
    output.zoomKick = CameraFeedbackClamp(output.zoomKick, 0.0f,
                                          CAMERA_MAX_ZOOM_KICK);
    return output;
}

Camera2D CameraFeedbackApplyTransient(Camera2D stableCamera,
                                      CameraFeedbackOutput output)
{
    Camera2D presentationCamera = stableCamera;
    float stableZoom = isfinite(stableCamera.zoom) && stableCamera.zoom >= 1.0f
                           ? stableCamera.zoom
                           : 1.0f;
    float zoomKick = isfinite(output.zoomKick)
                         ? CameraFeedbackClamp(output.zoomKick, 0.0f,
                                               CAMERA_MAX_ZOOM_KICK)
                         : 0.0f;

    /* Work on a copy: the stable transform remains the sole mouse-to-world
       contract while shake, rotation and zoom kick are presentation-only. */
    presentationCamera.target = Vector2Add(
        stableCamera.target,
        (Vector2){isfinite(output.impulseOffset.x) ? output.impulseOffset.x
                                                  : 0.0f,
                  isfinite(output.impulseOffset.y) ? output.impulseOffset.y
                                                  : 0.0f});
    presentationCamera.rotation =
        stableCamera.rotation +
        (isfinite(output.rotationDegrees)
             ? CameraFeedbackClamp(output.rotationDegrees,
                                   -CAMERA_MAX_ROTATION_DEGREES,
                                   CAMERA_MAX_ROTATION_DEGREES)
             : 0.0f);
    presentationCamera.zoom = fmaxf(1.0f, stableZoom / (1.0f + zoomKick));
    return presentationCamera;
}
