#ifndef CAMERA_FEEDBACK_H
#define CAMERA_FEEDBACK_H

/* Presentation-only camera motion.
 *
 * Gameplay publishes facts through GameEvent. This bounded controller turns
 * them into smooth camera impulses and combines them with velocity lookahead.
 * It never writes gameplay state and never uses frame-to-frame random jitter.
 */

#include <stdint.h>

#include <raylib.h>

#include "game_events.h"

#define CAMERA_IMPULSE_CAPACITY 16u

typedef struct CameraImpulse {
    Vector2 direction;
    float positionStrength;
    float rotationStrength;
    float zoomStrength;
    float duration;
    float age;
    float phase;
} CameraImpulse;

typedef struct CameraFeedbackStats {
    uint8_t active;
    uint8_t peak;
    uint32_t dropped;
} CameraFeedbackStats;

typedef struct CameraFeedback {
    CameraImpulse impulses[CAMERA_IMPULSE_CAPACITY];
    CameraFeedbackStats stats;
    Vector2 lookahead;
    float viewScale;
} CameraFeedback;

typedef struct CameraFeedbackMotion {
    Vector2 velocity;
    float normalSpeed;
    float maximumSpeed;
    float viewWidth;
    float viewHeight;
} CameraFeedbackMotion;

typedef struct CameraFeedbackOutput {
    Vector2 lookahead;
    Vector2 impulseOffset;
    float viewScale;
    float rotationDegrees;
    float zoomKick;
} CameraFeedbackOutput;

void CameraFeedbackInit(CameraFeedback *feedback);
void CameraFeedbackClear(CameraFeedback *feedback);
void CameraFeedbackConsumeEvents(CameraFeedback *feedback,
                                 const GameEventBuffer *events,
                                 Vector2 playerPosition);
CameraFeedbackOutput CameraFeedbackUpdate(CameraFeedback *feedback,
                                          CameraFeedbackMotion motion,
                                          float deltaTime);
Camera2D CameraFeedbackApplyTransient(Camera2D stableCamera,
                                      CameraFeedbackOutput output);

#endif
