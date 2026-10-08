// SPDX-License-Identifier: AGPL-3.0-or-later

#include "avatar_engine.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;
// All pose dimensions use a device-independent 1000 x 1000 design space.
// A 466 x 466 StopWatch therefore uses a 0.466 render scale.
constexpr float kDesignSize = 1000.0f;
constexpr uint32_t kFrameIntervalUs = 16667;
constexpr uint32_t kMetricsReportIntervalMs = 5000;
constexpr uint16_t kBackground = TFT_BLACK;
constexpr float kBackgroundBrightness = 0.0f;
constexpr float kEyeBrightness = 1.0f;
// Eye regions are rasterized in bands into internal RAM and copied to the
// display as finished pixels, so every frame writes each region exactly once
// instead of clearing it and overdrawing shapes.
constexpr int kBandWidth = 480;
constexpr int kBandPixels = kBandWidth * 16;
uint16_t bandPixels[kBandPixels];
constexpr float kTouchTravelX = 70.0f;
constexpr float kTouchTravelY = 56.0f;
constexpr float kTiltTravelX = 112.0f;
constexpr float kTiltTravelY = 92.0f;
constexpr float kTiltLeadTravelX = 34.0f;
constexpr float kTiltLeadTravelY = 28.0f;
constexpr float kShakeTravelX = 58.0f;
constexpr float kShakeTravelY = 44.0f;

AvatarEngine::EyePose makeEye(
    float x, float y, float width, float height, float roundness = 1.0f,
    float upperLid = 0.0f, float upperLidTilt = 0.0f,
    float lowerLid = 0.0f, float browY = -230.0f, float browTilt = 0.0f,
    float browOpacity = 0.0f, float angle = 0.0f) {
  return {x,          y,          width,    height,
          roundness, upperLid,   upperLidTilt, lowerLid,
          browY,     browTilt,   browOpacity, angle};
}

AvatarEngine::EyePose makeAngledEye(float x, float y, float width,
                                    float height, float angle,
                                    float roundness = 1.0f) {
  return makeEye(x, y, width, height, roundness, 0.0f, 0.0f, 0.0f,
                 -230.0f, 0.0f, 0.0f, angle);
}

AvatarEngine::Pose makePose(const AvatarEngine::EyePose& left,
                            const AvatarEngine::EyePose& right,
                            float faceScale = 1.0f, float faceX = 0.0f,
                            float faceY = -8.0f, float headYaw = 0.0f,
                            float headPitch = 0.0f,
                            float headRoll = 0.0f) {
  return {left, right, faceScale, faceX, faceY, headYaw, headPitch,
          headRoll};
}

AvatarEngine::Oscillator oscillator(float offset, float amplitude,
                                    float angularFrequency,
                                    float phase = 0.0f) {
  return {offset, amplitude, angularFrequency, phase};
}

AvatarEngine::MotionProfile makeMotion(
    const AvatarEngine::Oscillator& faceX,
    const AvatarEngine::Oscillator& faceY,
    const AvatarEngine::Oscillator& eyeX,
    const AvatarEngine::Oscillator& eyeY,
    const AvatarEngine::Oscillator& scale, uint16_t blendInMs = 180,
    uint16_t blendOutMs = 240) {
  return {faceX, faceY, eyeX, eyeY, scale, blendInMs, blendOutMs};
}

const AvatarEngine::MotionProfile kIdleMotion = makeMotion(
    oscillator(0.0f, 0.0f, 0.0f), oscillator(0.0f, 3.2f, 1.15f),
    oscillator(0.0f, 0.0f, 0.0f), oscillator(0.0f, 0.0f, 0.0f),
    oscillator(1.0f, 0.0045f, 1.05f));

const AvatarEngine::MotionProfile kHappyMotion = makeMotion(
    oscillator(0.0f, 0.0f, 0.0f), oscillator(0.0f, 15.0f, 8.0f),
    oscillator(0.0f, 3.5f, 1.1f),
    oscillator(0.0f, 2.0f, 0.9f, kPi * 0.5f),
    oscillator(1.0f, 0.008f, 2.4f));

const AvatarEngine::MotionProfile kAngryMotion = makeMotion(
    oscillator(0.0f, 9.0f, 32.0f), oscillator(1.5f, 2.0f, 3.0f),
    oscillator(0.0f, 3.6f, 32.0f), oscillator(0.0f, 0.8f, 2.0f),
    oscillator(1.0f, 0.004f, 2.0f), 150, 220);

const AvatarEngine::MotionProfile kSurprisedMotion = makeMotion(
    oscillator(0.0f, 1.5f, 2.2f), oscillator(-1.0f, 3.0f, 2.0f),
    oscillator(0.0f, 1.5f, 1.4f),
    oscillator(0.0f, 1.0f, 1.2f, kPi * 0.5f),
    oscillator(1.0f, 0.021f, 9.0f), 120, 260);

const AvatarEngine::MotionProfile kSleepyMotion = makeMotion(
    oscillator(0.0f, 0.0f, 0.0f), oscillator(9.0f, 4.0f, 0.9f),
    oscillator(0.0f, 0.0f, 0.0f), oscillator(0.0f, 0.0f, 0.0f),
    oscillator(1.0f, 0.003f, 0.7f), 320, 420);

const AvatarEngine::MotionProfile kDizzyMotion = makeMotion(
    oscillator(0.0f, 18.0f, 10.5f), oscillator(0.0f, 7.0f, 6.4f),
    oscillator(0.0f, 5.0f, 7.0f), oscillator(0.0f, 4.0f, 8.8f),
    oscillator(1.0f, 0.012f, 4.2f), 140, 300);

const AvatarEngine::MotionProfile kListeningMotion = makeMotion(
    oscillator(0.0f, 1.2f, 0.65f), oscillator(-1.0f, 2.5f, 0.85f),
    oscillator(0.0f, 2.4f, 0.72f), oscillator(0.0f, 1.5f, 0.58f),
    oscillator(1.0f, 0.0035f, 0.95f), 280, 360);

const AvatarEngine::MotionProfile kThinkingMotion = makeMotion(
    oscillator(0.0f, 2.8f, 0.52f), oscillator(-2.0f, 3.0f, 0.68f),
    oscillator(0.0f, 4.0f, 0.63f),
    oscillator(-2.0f, 2.6f, 0.49f, kPi * 0.5f),
    oscillator(1.0f, 0.003f, 0.72f), 320, 420);

const AvatarEngine::MotionProfile kCuriousMotion = makeMotion(
    oscillator(0.0f, 2.0f, 0.9f), oscillator(-1.0f, 3.5f, 1.15f),
    oscillator(0.0f, 3.0f, 0.8f), oscillator(0.0f, 2.0f, 0.7f),
    oscillator(1.0f, 0.006f, 1.25f), 160, 300);

const AvatarEngine::MotionProfile kConfusedMotion = makeMotion(
    oscillator(0.0f, 6.0f, 4.2f), oscillator(0.0f, 2.0f, 1.1f),
    oscillator(0.0f, 3.5f, 3.8f), oscillator(0.0f, 1.5f, 1.3f),
    oscillator(1.0f, 0.004f, 1.0f), 170, 320);

const AvatarEngine::MotionProfile kExcitedMotion = makeMotion(
    oscillator(0.0f, 3.5f, 6.8f), oscillator(0.0f, 15.0f, 8.5f),
    oscillator(0.0f, 4.0f, 5.2f), oscillator(0.0f, 3.0f, 4.8f),
    oscillator(1.0f, 0.014f, 6.2f), 110, 260);

const AvatarEngine::MotionProfile kSadMotion = makeMotion(
    oscillator(0.0f, 1.2f, 0.55f), oscillator(8.0f, 3.5f, 0.72f),
    oscillator(0.0f, 1.2f, 0.48f), oscillator(2.0f, 1.0f, 0.5f),
    oscillator(1.0f, 0.0025f, 0.62f), 300, 460);

constexpr AvatarEngine::BlinkSettings kNaturalBlink = {
    true, 2600, 3400, 6200, 112, 168};
constexpr AvatarEngine::BlinkSettings kExpressiveBlink = {
    true, 1200, 1800, 3600, 72, 112};
constexpr AvatarEngine::BlinkSettings kFocusedBlink = {
    true, 2100, 2800, 5000, 76, 116};
constexpr AvatarEngine::BlinkSettings kAttentiveBlink = {
    true, 1900, 2800, 5200, 95, 145};
constexpr AvatarEngine::BlinkSettings kThoughtfulBlink = {
    true, 2600, 4200, 6800, 120, 180};
constexpr AvatarEngine::BlinkSettings kSoftBlink = {
    true, 1800, 3200, 5600, 130, 190};
constexpr AvatarEngine::BlinkSettings kNoBlink = {
    false, 0, 0, 0, 0, 0};

const AvatarEngine::Keyframe kIdleFrames[] = {
    {makePose(makeEye(-225.0f, -15.0f, 155.0f, 360.0f),
              makeEye(225.0f, -15.0f, 155.0f, 360.0f)),
     260, 0, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kListeningFrames[] = {
    {makePose(makeAngledEye(-220.0f, -18.0f, 160.0f, 325.0f, -1.0f),
              makeAngledEye(220.0f, -18.0f, 160.0f, 325.0f, 1.0f),
              1.0f, 0.0f, -9.0f, 0.0f, -3.0f, 0.0f),
     360, 900, AvatarEngine::Easing::Smooth},
    {makePose(makeAngledEye(-218.0f, -22.0f, 152.0f, 315.0f, -3.0f),
              makeAngledEye(218.0f, -20.0f, 174.0f, 340.0f, 4.0f),
              1.01f, 0.0f, -10.0f, 12.0f, -5.0f, 2.0f),
     560, 1600, AvatarEngine::Easing::Smooth},
    {makePose(makeAngledEye(-220.0f, -18.0f, 172.0f, 338.0f, -4.0f),
              makeAngledEye(220.0f, -20.0f, 154.0f, 318.0f, 2.0f),
              1.0f, 0.0f, -9.0f, -9.0f, -4.0f, -1.5f),
     540, 1450, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kThinkingFrames[] = {
    {makePose(makeAngledEye(-220.0f, -30.0f, 178.0f, 305.0f, -5.0f),
              makeAngledEye(220.0f, -34.0f, 150.0f, 250.0f, 8.0f),
              1.0f, 0.0f, -12.0f, 18.0f, -14.0f, 4.0f),
     620, 1750, AvatarEngine::Easing::Smooth},
    {makePose(makeAngledEye(-218.0f, -28.0f, 152.0f, 245.0f, -9.0f),
              makeAngledEye(218.0f, -32.0f, 205.0f, 320.0f, 5.0f),
              1.0f, 0.0f, -12.0f, -15.0f, -11.0f, -5.0f),
     640, 1600, AvatarEngine::Easing::Smooth},
    {makePose(makeAngledEye(-220.0f, -38.0f, 188.0f, 280.0f, -3.0f),
              makeAngledEye(220.0f, -38.0f, 148.0f, 245.0f, 7.0f),
              1.005f, 0.0f, -14.0f, 7.0f, -17.0f, 3.0f),
     600, 1900, AvatarEngine::Easing::Smooth},
    {makePose(makeAngledEye(-218.0f, -26.0f, 148.0f, 250.0f, -8.0f),
              makeAngledEye(218.0f, -28.0f, 195.0f, 310.0f, 4.0f),
              1.0f, 0.0f, -11.0f, -18.0f, -8.0f, -4.0f),
     620, 1700, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kHappyFrames[] = {
    {makePose(makeEye(-225.0f, 5.0f, 180.0f, 285.0f),
              makeEye(225.0f, 5.0f, 180.0f, 285.0f), 0.98f, 0.0f, 4.0f),
     115, 0, AvatarEngine::Easing::Snappy},
    {makePose(makeEye(-220.0f, -4.0f, 290.0f, 72.0f, 1.0f, 0.0f, 0.0f,
                           0.0f, -112.0f, -8.0f, 0.28f),
              makeEye(220.0f, -4.0f, 290.0f, 72.0f, 1.0f, 0.0f, 0.0f,
                           0.0f, -112.0f, 8.0f, 0.28f),
              1.035f, 0.0f, -12.0f),
     225, 920, AvatarEngine::Easing::Spring},
    {makePose(makeEye(-220.0f, -20.0f, 270.0f, 86.0f),
              makeEye(220.0f, -20.0f, 270.0f, 86.0f), 1.015f, 0.0f,
              -16.0f),
     175, 420, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kExcitedFrames[] = {
    {makePose(makeAngledEye(-220.0f, 18.0f, 205.0f, 112.0f, -3.0f),
              makeAngledEye(220.0f, 18.0f, 205.0f, 112.0f, 3.0f),
              0.965f, 0.0f, 10.0f, 0.0f, 10.0f, 0.0f),
     90, 25, AvatarEngine::Easing::Snappy},
    {makePose(makeAngledEye(-225.0f, -34.0f, 225.0f, 365.0f, -6.0f),
              makeAngledEye(225.0f, -34.0f, 225.0f, 365.0f, 6.0f),
              1.075f, 0.0f, -24.0f, 0.0f, -11.0f, 0.0f),
     155, 180, AvatarEngine::Easing::Spring},
    {makePose(makeAngledEye(-220.0f, -16.0f, 205.0f, 330.0f, -10.0f),
              makeAngledEye(220.0f, -16.0f, 205.0f, 330.0f, 2.0f),
              1.035f, 0.0f, -12.0f, -7.0f, -5.0f, -4.5f),
     135, 120, AvatarEngine::Easing::Spring},
    {makePose(makeEye(-220.0f, -12.0f, 285.0f, 82.0f),
              makeEye(220.0f, -12.0f, 285.0f, 82.0f), 1.035f, 0.0f,
              -16.0f, 7.0f, -7.0f, 4.0f),
     170, 720, AvatarEngine::Easing::Spring},
};

const AvatarEngine::Keyframe kCuriousFrames[] = {
    {makePose(makeAngledEye(-220.0f, -16.0f, 160.0f, 320.0f, -2.0f),
              makeAngledEye(220.0f, -16.0f, 160.0f, 320.0f, 2.0f),
              0.99f, 0.0f, -6.0f, 0.0f, 2.0f, 0.0f),
     120, 40, AvatarEngine::Easing::Snappy},
    {makePose(makeAngledEye(-218.0f, -28.0f, 195.0f, 360.0f, -8.0f),
              makeAngledEye(218.0f, -18.0f, 142.0f, 270.0f, 4.0f),
              1.025f, 0.0f, -14.0f, 16.0f, -8.0f, 7.0f),
     230, 850, AvatarEngine::Easing::Spring},
    {makePose(makeAngledEye(-220.0f, -22.0f, 182.0f, 342.0f, -6.0f),
              makeAngledEye(220.0f, -18.0f, 150.0f, 292.0f, 3.0f),
              1.01f, 0.0f, -11.0f, 10.0f, -5.0f, 5.0f),
     260, 520, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kConfusedFrames[] = {
    {makePose(makeAngledEye(-220.0f, -12.0f, 188.0f, 290.0f, 9.0f),
              makeAngledEye(220.0f, -26.0f, 145.0f, 250.0f, -7.0f),
              0.99f, 0.0f, -5.0f, -12.0f, 5.0f, -5.0f),
     165, 300, AvatarEngine::Easing::Snappy},
    {makePose(makeAngledEye(-218.0f, -26.0f, 145.0f, 250.0f, 7.0f),
              makeAngledEye(218.0f, -12.0f, 188.0f, 290.0f, -9.0f),
              1.005f, 0.0f, -8.0f, 12.0f, 3.0f, 5.0f),
     280, 420, AvatarEngine::Easing::Spring},
    {makePose(makeEye(-220.0f, -20.0f, 178.0f, 245.0f, 0.9f, 0.14f,
                           -0.32f, 0.0f, -195.0f, -10.0f, 0.62f),
              makeEye(220.0f, -20.0f, 178.0f, 245.0f, 0.9f, 0.14f,
                           0.32f, 0.0f, -195.0f, 10.0f, 0.62f),
              1.0f, 0.0f, -6.0f, 0.0f, 4.0f, 0.0f),
     260, 720, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kAngryFrames[] = {
    {makePose(makeEye(-220.0f, -18.0f, 205.0f, 245.0f, 0.8f, 0.15f,
                           0.45f, 0.0f, -190.0f, 17.0f, 0.72f),
              makeEye(220.0f, -18.0f, 205.0f, 245.0f, 0.8f, 0.15f,
                           -0.45f, 0.0f, -190.0f, -17.0f, 0.72f),
              0.985f, 0.0f, 4.0f),
     145, 80, AvatarEngine::Easing::Snappy},
    {makePose(makeEye(-212.0f, -28.0f, 220.0f, 270.0f, 0.72f, 0.22f,
                           0.55f, 0.0f, -205.0f, 20.0f, 1.0f),
              makeEye(212.0f, -28.0f, 220.0f, 270.0f, 0.72f, 0.22f,
                           -0.55f, 0.0f, -205.0f, -20.0f, 1.0f),
              1.015f, 0.0f, 4.0f),
     230, 1160, AvatarEngine::Easing::Spring},
    {makePose(makeEye(-218.0f, -20.0f, 208.0f, 248.0f, 0.76f, 0.18f,
                           0.48f, 0.0f, -194.0f, 18.0f, 0.85f),
              makeEye(218.0f, -20.0f, 208.0f, 248.0f, 0.76f, 0.18f,
                           -0.48f, 0.0f, -194.0f, -18.0f, 0.85f),
              1.0f, 0.0f, 3.0f),
     190, 350, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kSurprisedFrames[] = {
    {makePose(makeEye(-225.0f, 5.0f, 205.0f, 92.0f),
              makeEye(225.0f, 5.0f, 205.0f, 92.0f), 0.97f, 0.0f, 2.0f),
     105, 0, AvatarEngine::Easing::Snappy},
    {makePose(makeEye(-230.0f, -15.0f, 255.0f, 255.0f),
              makeEye(230.0f, -15.0f, 255.0f, 255.0f), 1.06f, 0.0f,
              -12.0f),
     215, 810, AvatarEngine::Easing::Spring},
    {makePose(makeEye(-228.0f, -14.0f, 225.0f, 275.0f),
              makeEye(228.0f, -14.0f, 225.0f, 275.0f), 1.025f, 0.0f,
              -10.0f),
     180, 430, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kSadFrames[] = {
    {makePose(makeEye(-220.0f, -5.0f, 180.0f, 275.0f, 0.95f, 0.08f,
                           -0.20f, 0.0f, -210.0f, -12.0f, 0.55f),
              makeEye(220.0f, -5.0f, 180.0f, 275.0f, 0.95f, 0.08f,
                           0.20f, 0.0f, -210.0f, 12.0f, 0.55f),
              0.995f, 0.0f, 4.0f, 0.0f, 5.0f, 0.0f),
     280, 260, AvatarEngine::Easing::Smooth},
    {makePose(makeEye(-215.0f, 18.0f, 205.0f, 205.0f, 0.9f, 0.26f,
                           -0.38f, 0.06f, -205.0f, -17.0f, 0.92f),
              makeEye(215.0f, 18.0f, 205.0f, 205.0f, 0.9f, 0.26f,
                           0.38f, 0.06f, -205.0f, 17.0f, 0.92f),
              0.985f, 0.0f, 20.0f, 0.0f, 15.0f, 0.0f),
     430, 1250, AvatarEngine::Easing::Smooth},
    {makePose(makeEye(-215.0f, 22.0f, 215.0f, 165.0f, 0.92f, 0.30f,
                           -0.42f, 0.08f, -190.0f, -18.0f, 1.0f),
              makeEye(215.0f, 22.0f, 215.0f, 165.0f, 0.92f, 0.30f,
                           0.42f, 0.08f, -190.0f, 18.0f, 1.0f),
              0.98f, 0.0f, 24.0f, -5.0f, 18.0f, -2.5f),
     360, 760, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kSleepyFrames[] = {
    {makePose(makeEye(-215.0f, 18.0f, 245.0f, 165.0f, 1.0f, 0.28f),
              makeEye(215.0f, 18.0f, 245.0f, 165.0f, 1.0f, 0.28f),
              0.99f, 0.0f, 18.0f),
     320, 380, AvatarEngine::Easing::Smooth},
    {makePose(makeEye(-210.0f, 48.0f, 305.0f, 48.0f),
              makeEye(210.0f, 48.0f, 305.0f, 48.0f), 0.98f, 0.0f,
              26.0f),
     520, 1380, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kDizzyFrames[] = {
    {makePose(makeEye(-210.0f, -10.0f, 270.0f, 270.0f),
              makeEye(210.0f, 4.0f, 244.0f, 244.0f), 1.015f, 0.0f,
              -5.0f, 0.0f, 0.0f, -4.0f),
     180, 150, AvatarEngine::Easing::Spring},
    {makePose(makeEye(-210.0f, 5.0f, 242.0f, 242.0f),
              makeEye(210.0f, -12.0f, 274.0f, 274.0f), 1.025f, 0.0f,
              -2.0f, 0.0f, 0.0f, 5.0f),
     210, 150, AvatarEngine::Easing::Spring},
    {makePose(makeEye(-210.0f, -7.0f, 264.0f, 264.0f),
              makeEye(210.0f, 2.0f, 252.0f, 252.0f), 1.015f, 0.0f,
              -5.0f, 0.0f, 0.0f, -3.5f),
     210, 360, AvatarEngine::Easing::Spring},
};

const AvatarEngine::ExpressionSpec kExpressions[] = {
    {"IDLE", kIdleFrames, 1, AvatarEngine::PlaybackMode::Loop,
     kNaturalBlink, kIdleMotion, true},
    {"LISTENING", kListeningFrames, 3, AvatarEngine::PlaybackMode::Loop,
     kAttentiveBlink, kListeningMotion, true},
    {"THINKING", kThinkingFrames, 4, AvatarEngine::PlaybackMode::Loop,
     kThoughtfulBlink, kThinkingMotion, true},
    {"HAPPY", kHappyFrames, 3, AvatarEngine::PlaybackMode::Once,
     kExpressiveBlink, kHappyMotion, false},
    {"EXCITED", kExcitedFrames, 4, AvatarEngine::PlaybackMode::Once,
     kExpressiveBlink, kExcitedMotion, false},
    {"CURIOUS", kCuriousFrames, 3, AvatarEngine::PlaybackMode::Once,
     kAttentiveBlink, kCuriousMotion, false},
    {"CONFUSED", kConfusedFrames, 3, AvatarEngine::PlaybackMode::Once,
     kFocusedBlink, kConfusedMotion, false},
    {"ANGRY", kAngryFrames, 3, AvatarEngine::PlaybackMode::Once,
     kFocusedBlink, kAngryMotion, false},
    {"SURPRISED", kSurprisedFrames, 3, AvatarEngine::PlaybackMode::Once,
     kNoBlink, kSurprisedMotion, false},
    {"SAD", kSadFrames, 3, AvatarEngine::PlaybackMode::Once,
     kSoftBlink, kSadMotion, false},
    {"SLEEPY", kSleepyFrames, 2, AvatarEngine::PlaybackMode::Once,
     kNoBlink, kSleepyMotion, false},
    {"DIZZY", kDizzyFrames, 3, AvatarEngine::PlaybackMode::Once,
     kNoBlink, kDizzyMotion, false},
};

static_assert(sizeof(kExpressions) / sizeof(kExpressions[0]) ==
                  static_cast<size_t>(ExpressionId::Count),
              "Expression catalog must match ExpressionId");

float clamp01(float value) {
  return std::max(0.0f, std::min(1.0f, value));
}

float smootherStep(float value) {
  const float progress = clamp01(value);
  return progress * progress * progress *
         (progress * (progress * 6.0f - 15.0f) + 10.0f);
}

struct AttentionPose {
  float yaw;
  float pitch;
  float roll;
  float eyeX;
  float eyeY;
  uint16_t holdMs;
  uint16_t transitionMs;
};

const AttentionPose kIdleAttention[] = {
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 4200, 620},
    {24.0f, -10.0f, 4.8f, 12.0f, -5.0f, 5200, 560},
    {-20.0f, 14.0f, -5.5f, -10.0f, 7.0f, 3600, 520},
    {11.0f, -16.0f, 2.2f, 6.0f, -8.0f, 4500, 500},
    {-27.0f, -4.0f, -4.0f, -12.0f, 1.0f, 5000, 640},
};

float mixFloat(float from, float to, float amount) {
  return from + (to - from) * amount;
}

AttentionPose mixAttention(const AttentionPose& from,
                           const AttentionPose& to, float amount) {
  return {mixFloat(from.yaw, to.yaw, amount),
          mixFloat(from.pitch, to.pitch, amount),
          mixFloat(from.roll, to.roll, amount),
          mixFloat(from.eyeX, to.eyeX, amount),
          mixFloat(from.eyeY, to.eyeY, amount), 0, 0};
}

AttentionPose sampleIdleAttention(uint32_t nowMs) {
  uint32_t cycleMs = 0;
  for (const auto& pose : kIdleAttention) {
    cycleMs += pose.holdMs + pose.transitionMs;
  }
  uint32_t cursor = cycleMs == 0 ? 0 : nowMs % cycleMs;
  const size_t count = sizeof(kIdleAttention) / sizeof(kIdleAttention[0]);
  for (size_t index = 0; index < count; ++index) {
    const AttentionPose& pose = kIdleAttention[index];
    if (cursor < pose.holdMs) return pose;
    cursor -= pose.holdMs;
    if (cursor < pose.transitionMs) {
      const float progress = smootherStep(
          cursor / static_cast<float>(std::max<uint16_t>(1,
                                                         pose.transitionMs)));
      return mixAttention(pose, kIdleAttention[(index + 1) % count], progress);
    }
    cursor -= pose.transitionMs;
  }
  return kIdleAttention[0];
}

float attentionHash(uint32_t value, float seed) {
  const float raw = sinf(value * 127.1f + seed * 311.7f) * 43758.5453f;
  return (raw - floorf(raw)) * 2.0f - 1.0f;
}

float microSaccade(uint32_t nowMs, float seed) {
  constexpr uint32_t kIntervalMs = 1100;
  constexpr uint32_t kMoveMs = 145;
  const uint32_t step = nowMs / kIntervalMs;
  const uint32_t elapsed = nowMs % kIntervalMs;
  const float previous = step == 0 ? 0.0f : attentionHash(step - 1, seed);
  const float next = attentionHash(step, seed);
  const float progress = smootherStep(
      std::min(1.0f, elapsed / static_cast<float>(kMoveMs)));
  return mixFloat(previous, next, progress);
}

float sampleOscillator(const AvatarEngine::Oscillator& oscillator,
                       float time) {
  if (oscillator.amplitude == 0.0f || oscillator.angularFrequency == 0.0f) {
    return oscillator.offset;
  }
  return oscillator.offset +
         sinf(time * oscillator.angularFrequency + oscillator.phase) *
             oscillator.amplitude;
}

struct MotionSample {
  float faceX;
  float faceY;
  float eyeX;
  float eyeY;
  float scale;
};

MotionSample sampleMotion(const AvatarEngine::MotionProfile& profile,
                          float time) {
  return {sampleOscillator(profile.faceX, time),
          sampleOscillator(profile.faceY, time),
          sampleOscillator(profile.eyeX, time),
          sampleOscillator(profile.eyeY, time),
          sampleOscillator(profile.scale, time)};
}

MotionSample mixMotion(const MotionSample& from, const MotionSample& to,
                       float amount) {
  const auto mix = [amount](float a, float b) {
    return a + (b - a) * amount;
  };
  return {mix(from.faceX, to.faceX), mix(from.faceY, to.faceY),
          mix(from.eyeX, to.eyeX), mix(from.eyeY, to.eyeY),
          mix(from.scale, to.scale)};
}

void projectEyeOntoHead(AvatarEngine::EyePose& eye, float side,
                        float headYaw, float headPitch, float headRoll) {
  const float yaw = std::max(-1.0f, std::min(1.0f, headYaw / 35.0f));
  const float pitch =
      std::max(-1.0f, std::min(1.0f, headPitch / 28.0f));
  const float horizontalCompression = 1.0f - fabsf(yaw) * 0.16f;
  const float depthScale =
      std::max(0.74f, 1.0f + side * yaw * 0.16f - fabsf(pitch) * 0.03f);

  eye.x = eye.x * horizontalCompression + yaw * 82.0f;
  eye.y += pitch * 62.0f;
  eye.width *= depthScale * (1.0f - fabsf(yaw) * 0.10f);
  eye.height *= depthScale * (1.0f - fabsf(pitch) * 0.05f);

  const float rollRadians = headRoll * kPi / 180.0f;
  const float rotatedX =
      eye.x * cosf(rollRadians) - eye.y * sinf(rollRadians);
  const float rotatedY =
      eye.x * sinf(rollRadians) + eye.y * cosf(rollRadians);
  eye.x = rotatedX;
  eye.y = rotatedY;
  eye.angle += headRoll + yaw * 7.0f + side * yaw * 3.5f;
}

bool reached(uint32_t now, uint32_t deadline) {
  return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace

bool AvatarEngine::begin() {
  ready_ = M5.Display.width() > 0 && M5.Display.height() > 0;
  targetExpression_ = ExpressionId::Idle;
  baseExpression_ = ExpressionId::Idle;
  browseExpression_ = ExpressionId::Idle;
  activePlaybackMode_ = spec(ExpressionId::Idle).defaultPlaybackMode;
  currentPose_ = spec(ExpressionId::Idle).keyframes[0].pose;
  fromPose_ = currentPose_;
  targetPose_ = currentPose_;
  const uint32_t nowMs = millis();
  expressionStartedMs_ = nowMs;
  scheduleNextBlink(nowMs, true);
  metricsStartedMs_ = nowMs;
  forceRender_ = true;
  requiresFullClear_ = true;
  Serial.printf("Avatar renderer: %d x %d, normalized scale %.3f, dirty rects\n",
                M5.Display.width(), M5.Display.height(),
                std::min(M5.Display.width(), M5.Display.height()) / kDesignSize);
  return ready_;
}

const AvatarEngine::ExpressionSpec& AvatarEngine::spec(ExpressionId expression) {
  const size_t index = std::min(static_cast<size_t>(expression),
                                static_cast<size_t>(ExpressionId::Count) - 1);
  return kExpressions[index];
}

const char* AvatarEngine::activeName() const {
  return spec(targetExpression_).name;
}

float AvatarEngine::ease(Easing easingMode, float progress) {
  const float value = clamp01(progress);
  if (easingMode == Easing::Snappy) {
    const float inverse = 1.0f - value;
    return 1.0f - inverse * inverse * inverse;
  }
  if (easingMode == Easing::Spring) {
    if (value >= 1.0f) return 1.0f;
    return 1.0f - expf(-6.5f * value) * cosf(10.0f * value);
  }
  return smootherStep(value);
}

AvatarEngine::EyePose AvatarEngine::interpolateEye(const EyePose& from,
                                                    const EyePose& to,
                                                    float amount) {
  const auto mix = [amount](float a, float b) { return a + (b - a) * amount; };
  return {mix(from.x, to.x),
          mix(from.y, to.y),
          mix(from.width, to.width),
          mix(from.height, to.height),
          mix(from.roundness, to.roundness),
          mix(from.upperLid, to.upperLid),
          mix(from.upperLidTilt, to.upperLidTilt),
          mix(from.lowerLid, to.lowerLid),
          mix(from.browY, to.browY),
          mix(from.browTilt, to.browTilt),
          mix(from.browOpacity, to.browOpacity),
          mix(from.angle, to.angle)};
}

AvatarEngine::Pose AvatarEngine::interpolate(const Pose& from, const Pose& to,
                                              float progress,
                                              Easing easingMode) {
  const float amount = ease(easingMode, progress);
  const auto mix = [amount](float a, float b) { return a + (b - a) * amount; };
  return {interpolateEye(from.leftEye, to.leftEye, amount),
          interpolateEye(from.rightEye, to.rightEye, amount),
          mix(from.faceScale, to.faceScale), mix(from.faceX, to.faceX),
          mix(from.faceY, to.faceY), mix(from.headYaw, to.headYaw),
          mix(from.headPitch, to.headPitch),
          mix(from.headRoll, to.headRoll)};
}

void AvatarEngine::show(ExpressionId expression, uint32_t nowMs,
                        bool autoReturn, uint16_t firstTransitionMs) {
  if (!ready_) return;

  browseExpression_ = expression;
  const ExpressionSpec& expressionSpec = spec(expression);
  if (expressionSpec.persistent) {
    baseExpression_ = expression;
    play(expression, nowMs, expressionSpec.defaultPlaybackMode, false,
         firstTransitionMs);
    return;
  }

  const PlaybackMode mode =
      autoReturn ? PlaybackMode::Once : expressionSpec.defaultPlaybackMode;
  play(expression, nowMs, mode, autoReturn, firstTransitionMs);
}

void AvatarEngine::play(ExpressionId expression, uint32_t nowMs,
                        PlaybackMode mode, bool returnToBase,
                        uint16_t firstTransitionMs) {
  if (!ready_) return;

  targetExpression_ = expression;
  returnToBase_ = returnToBase && !spec(expression).persistent;
  activePlaybackMode_ = returnToBase_ ? PlaybackMode::Once : mode;
  playbackDirection_ = 1;
  playbackComplete_ = false;
  expressionStartedMs_ = nowMs;
  expressionEndsAtMs_ = 0;
  if (returnToBase_) {
    const ExpressionSpec& expressionSpec = spec(expression);
    expressionEndsAtMs_ = nowMs;
    for (uint8_t index = 0; index < expressionSpec.keyframeCount; ++index) {
      expressionEndsAtMs_ += expressionSpec.keyframes[index].transitionMs +
                             expressionSpec.keyframes[index].holdMs;
    }
  }
  scheduleNextBlink(nowMs, true);
  startKeyframe(0, nowMs, firstTransitionMs);
  forceRender_ = true;
}

void AvatarEngine::startKeyframe(uint8_t index, uint32_t nowMs,
                                 uint16_t transitionOverrideMs) {
  const ExpressionSpec& expression = spec(targetExpression_);
  activeKeyframeIndex_ = std::min<uint8_t>(index, expression.keyframeCount - 1);
  const Keyframe& keyframe = expression.keyframes[activeKeyframeIndex_];
  fromPose_ = currentPose_;
  targetPose_ = keyframe.pose;
  transitionStartedMs_ = nowMs;
  transitionDurationMs_ = transitionOverrideMs == 0
                              ? keyframe.transitionMs
                              : transitionOverrideMs;
  transitionEasing_ = keyframe.easing;
}

void AvatarEngine::advanceTimeline(uint32_t nowMs) {
  if (playbackComplete_) return;

  const ExpressionSpec& expression = spec(targetExpression_);
  const Keyframe& keyframe = expression.keyframes[activeKeyframeIndex_];
  const uint32_t keyframeEndsAt =
      transitionStartedMs_ + transitionDurationMs_ + keyframe.holdMs;
  if (!reached(nowMs, keyframeEndsAt)) return;

  const int nextIndex =
      static_cast<int>(activeKeyframeIndex_) + playbackDirection_;
  if (nextIndex >= 0 && nextIndex < expression.keyframeCount) {
    startKeyframe(static_cast<uint8_t>(nextIndex), nowMs);
    return;
  }

  if (activePlaybackMode_ == PlaybackMode::Loop) {
    playbackDirection_ = 1;
    startKeyframe(0, nowMs);
  } else if (activePlaybackMode_ == PlaybackMode::PingPong &&
             expression.keyframeCount > 1) {
    playbackDirection_ = -playbackDirection_;
    const int reflectedIndex =
        static_cast<int>(activeKeyframeIndex_) + playbackDirection_;
    startKeyframe(static_cast<uint8_t>(reflectedIndex), nowMs);
  } else {
    completePlayback(nowMs);
  }
}

void AvatarEngine::completePlayback(uint32_t nowMs) {
  playbackComplete_ = true;
  if (returnToBase_) {
    Serial.printf("Return to base: %s\n", spec(baseExpression_).name);
    play(baseExpression_, nowMs, spec(baseExpression_).defaultPlaybackMode,
         false);
  }
}

ExpressionId AvatarEngine::adjacentExpression(int8_t direction) const {
  int index = static_cast<int>(browseExpression_) + (direction < 0 ? -1 : 1);
  if (index >= static_cast<int>(ExpressionId::Count)) index = 1;
  if (index <= 0) index = static_cast<int>(ExpressionId::Count) - 1;
  return static_cast<ExpressionId>(index);
}

void AvatarEngine::next(uint32_t nowMs, uint16_t firstTransitionMs) {
  show(adjacentExpression(1), nowMs, true, firstTransitionMs);
}

void AvatarEngine::previous(uint32_t nowMs, uint16_t firstTransitionMs) {
  show(adjacentExpression(-1), nowMs, true, firstTransitionMs);
}

bool AvatarEngine::showFromCommand(const String& rawCommand, uint32_t nowMs) {
  String command = rawCommand;
  command.trim();
  command.toLowerCase();

  PlaybackMode requestedMode = PlaybackMode::Once;
  bool hasPlaybackOverride = false;
  if (command.startsWith("loop ")) {
    requestedMode = PlaybackMode::Loop;
    hasPlaybackOverride = true;
    command.remove(0, 5);
    command.trim();
  } else if (command.startsWith("pingpong ")) {
    requestedMode = PlaybackMode::PingPong;
    hasPlaybackOverride = true;
    command.remove(0, 9);
    command.trim();
  } else if (command.startsWith("once ")) {
    requestedMode = PlaybackMode::Once;
    hasPlaybackOverride = true;
    command.remove(0, 5);
    command.trim();
  }

  ExpressionId expression = ExpressionId::Idle;
  if (command == "idle" || command == "neutral") {
    expression = ExpressionId::Idle;
  } else if (command == "listening" || command == "listen") {
    expression = ExpressionId::Listening;
  } else if (command == "thinking" || command == "think") {
    expression = ExpressionId::Thinking;
  } else if (command == "happy" || command == "smile") {
    expression = ExpressionId::Happy;
  } else if (command == "excited" || command == "excite") {
    expression = ExpressionId::Excited;
  } else if (command == "curious" || command == "curiosity") {
    expression = ExpressionId::Curious;
  } else if (command == "confused" || command == "confuse") {
    expression = ExpressionId::Confused;
  } else if (command == "angry") {
    expression = ExpressionId::Angry;
  } else if (command == "surprised" || command == "surprise") {
    expression = ExpressionId::Surprised;
  } else if (command == "sad") {
    expression = ExpressionId::Sad;
  } else if (command == "sleepy" || command == "sleep") {
    expression = ExpressionId::Sleepy;
  } else if (command == "dizzy") {
    expression = ExpressionId::Dizzy;
  } else {
    return false;
  }

  if (hasPlaybackOverride) {
    play(expression, nowMs, requestedMode, false);
  } else {
    show(expression, nowMs, !spec(expression).persistent);
  }
  return true;
}

void AvatarEngine::setTouchTarget(int16_t screenX, int16_t screenY) {
  if (!ready_) return;
  const float halfWidth = std::max(1.0f, M5.Display.width() * 0.5f);
  const float halfHeight = std::max(1.0f, M5.Display.height() * 0.5f);
  const float normalizedX = std::max(
      -1.0f, std::min(1.0f, (screenX - halfWidth) / halfWidth));
  const float normalizedY = std::max(
      -1.0f, std::min(1.0f, (screenY - halfHeight) / halfHeight));
  touchTargetX_ = normalizedX * kTouchTravelX;
  touchTargetY_ = normalizedY * kTouchTravelY;
  touchActive_ = true;
}

void AvatarEngine::releaseTouch() {
  touchActive_ = false;
}

void AvatarEngine::setSwipeOffset(float screenDeltaX, float screenDeltaY,
                                  int8_t previewDirection) {
  swipeActive_ = true;
  swipeTargetX_ = std::max(-130.0f, std::min(130.0f, screenDeltaX * 0.96f));
  swipeTargetY_ = std::max(-115.0f, std::min(115.0f, screenDeltaY * 0.90f));
  swipePreviewTarget_ = previewDirection == 0
                            ? 0.0f
                            : std::min(0.34f,
                                       fabsf(screenDeltaX) / 52.0f * 0.34f);
  swipePreviewDirection_ = previewDirection;
}

void AvatarEngine::releaseSwipe() {
  swipeActive_ = false;
  swipeTargetX_ = 0.0f;
  swipeTargetY_ = 0.0f;
  swipePreviewTarget_ = 0.0f;
}

void AvatarEngine::commitSwipe(int8_t direction, uint32_t nowMs,
                               uint16_t firstTransitionMs) {
  if (!ready_ || direction == 0) return;

  const ExpressionId target = adjacentExpression(direction);
  const Pose& previewPose = spec(target).keyframes[0].pose;
  if (swipePreviewAmount_ > 0.01f) {
    currentPose_ = interpolate(currentPose_, previewPose,
                               swipePreviewAmount_, Easing::Smooth);
  }
  // Keep the swipe offset active after the expression changes. The same
  // finger may continue moving, so the new face must remain attached to it
  // until releaseSwipe() is called on finger-up.
  swipePreviewAmount_ = 0.0f;
  swipePreviewTarget_ = 0.0f;
  swipePreviewDirection_ = 0;
  show(target, nowMs, true, firstTransitionMs);
}

void AvatarEngine::setTiltTarget(float normalizedX, float normalizedY,
                                 float motionLeadX, float motionLeadY) {
  tiltTargetX_ = std::max(-1.0f, std::min(1.0f, normalizedX)) * kTiltTravelX;
  tiltTargetY_ = std::max(-1.0f, std::min(1.0f, normalizedY)) * kTiltTravelY;
  tiltLeadTargetX_ =
      std::max(-1.0f, std::min(1.0f, motionLeadX)) * kTiltLeadTravelX;
  tiltLeadTargetY_ =
      std::max(-1.0f, std::min(1.0f, motionLeadY)) * kTiltLeadTravelY;
}

void AvatarEngine::setShakeTarget(float normalizedX, float normalizedY,
                                  float intensity) {
  shakeIntensityTarget_ = clamp01(intensity);
  shakeTargetX_ = std::max(-1.0f, std::min(1.0f, normalizedX)) *
                  kShakeTravelX * shakeIntensityTarget_;
  shakeTargetY_ = std::max(-1.0f, std::min(1.0f, normalizedY)) *
                  kShakeTravelY * shakeIntensityTarget_;
}

void AvatarEngine::updateInteraction(uint32_t nowMs) {
  if (lastInteractionUpdateMs_ == 0) {
    lastInteractionUpdateMs_ = nowMs;
    return;
  }

  const float elapsedSeconds = std::min(
      0.033f, std::max(0.001f, (nowMs - lastInteractionUpdateMs_) / 1000.0f));
  lastInteractionUpdateMs_ = nowMs;

  const float targetX =
      touchActive_ ? touchTargetX_ : tiltTargetX_ + tiltLeadTargetX_;
  const float targetY =
      touchActive_ ? touchTargetY_ : tiltTargetY_ + tiltLeadTargetY_;
  // A damped spring gives direct tracking while pressed and a short, visible
  // rebound when the finger leaves the glass. The values are in design space,
  // so the same feel is retained if the display resolution changes.
  const float stiffness = touchActive_ ? 150.0f : 185.0f;
  const float damping = touchActive_ ? 21.0f : 25.5f;
  interactionVelocityX_ +=
      ((targetX - interactionX_) * stiffness -
       interactionVelocityX_ * damping) * elapsedSeconds;
  interactionVelocityY_ +=
      ((targetY - interactionY_) * stiffness -
       interactionVelocityY_ * damping) * elapsedSeconds;
  interactionX_ += interactionVelocityX_ * elapsedSeconds;
  interactionY_ += interactionVelocityY_ * elapsedSeconds;

  // High-pass IMU acceleration drives a faster spring than ordinary tilt.
  // The small overshoot is intentional: the face appears to have mass instead
  // of merely copying the device position at the instant a shake begins.
  constexpr float kShakeStiffness = 235.0f;
  constexpr float kShakeDamping = 18.0f;
  shakeVelocityX_ +=
      ((shakeTargetX_ - shakeOffsetX_) * kShakeStiffness -
       shakeVelocityX_ * kShakeDamping) * elapsedSeconds;
  shakeVelocityY_ +=
      ((shakeTargetY_ - shakeOffsetY_) * kShakeStiffness -
       shakeVelocityY_ * kShakeDamping) * elapsedSeconds;
  shakeOffsetX_ += shakeVelocityX_ * elapsedSeconds;
  shakeOffsetY_ += shakeVelocityY_ * elapsedSeconds;
  shakeIntensity_ +=
      (shakeIntensityTarget_ - shakeIntensity_) *
      std::min(1.0f, elapsedSeconds * 13.0f);

  const float normalizedX = touchActive_
                                ? touchTargetX_ / kTouchTravelX
                                : tiltTargetX_ / kTiltTravelX;
  const float normalizedY = touchActive_
                                ? touchTargetY_ / kTouchTravelY
                                : tiltTargetY_ / kTiltTravelY;
  const float targetYaw = normalizedX * (touchActive_ ? 32.0f : 21.0f);
  const float targetPitch = normalizedY * (touchActive_ ? 24.0f : 16.0f);
  const float targetRoll = -normalizedX * (touchActive_ ? 7.0f : 4.0f);
  const float headStiffness = touchActive_ ? 46.0f : 15.0f;
  const float headDamping = touchActive_ ? 11.5f : 7.0f;

  const auto updateHeadAxis = [elapsedSeconds, headStiffness, headDamping](
                                  float target, float& value,
                                  float& velocity) {
    velocity += ((target - value) * headStiffness - velocity * headDamping) *
                elapsedSeconds;
    value += velocity * elapsedSeconds;
  };
  updateHeadAxis(targetYaw, headYaw_, headYawVelocity_);
  updateHeadAxis(targetPitch, headPitch_, headPitchVelocity_);
  updateHeadAxis(targetRoll, headRoll_, headRollVelocity_);

  if (swipeActive_) {
    swipeVelocityX_ = std::max(
        -900.0f, std::min(900.0f,
                          (swipeTargetX_ - swipeOffsetX_) / elapsedSeconds));
    swipeVelocityY_ = std::max(
        -900.0f, std::min(900.0f,
                          (swipeTargetY_ - swipeOffsetY_) / elapsedSeconds));
    swipeOffsetX_ = swipeTargetX_;
    swipeOffsetY_ = swipeTargetY_;
    swipePreviewAmount_ = swipePreviewTarget_;
  } else {
    constexpr float kSwipeReturnStiffness = 190.0f;
    constexpr float kSwipeReturnDamping = 24.0f;
    swipeVelocityX_ +=
        ((swipeTargetX_ - swipeOffsetX_) * kSwipeReturnStiffness -
         swipeVelocityX_ * kSwipeReturnDamping) * elapsedSeconds;
    swipeVelocityY_ +=
        ((swipeTargetY_ - swipeOffsetY_) * kSwipeReturnStiffness -
         swipeVelocityY_ * kSwipeReturnDamping) * elapsedSeconds;
    swipeOffsetX_ += swipeVelocityX_ * elapsedSeconds;
    swipeOffsetY_ += swipeVelocityY_ * elapsedSeconds;
    swipePreviewAmount_ +=
        (swipePreviewTarget_ - swipePreviewAmount_) *
        std::min(1.0f, elapsedSeconds * 18.0f);
    if (swipePreviewAmount_ < 0.005f) swipePreviewDirection_ = 0;
  }

  const float tiltMagnitude =
      std::min(1.0f, sqrtf(normalizedX * normalizedX +
                           normalizedY * normalizedY));
  const float tiltControl = clamp01((tiltMagnitude - 0.06f) / 0.40f);
  const float attentionTarget =
      touchActive_ ? 0.08f : 1.0f - tiltControl * 0.90f;
  attentionWeight_ +=
      (attentionTarget - attentionWeight_) *
      std::min(1.0f, elapsedSeconds * (touchActive_ ? 8.0f : 3.5f));
}

void AvatarEngine::invalidate() {
  forceRender_ = true;
  requiresFullClear_ = true;
  previousLeftBounds_.valid = false;
  previousRightBounds_.valid = false;
  nextFrameUs_ = 0;
}

void AvatarEngine::scheduleNextBlink(uint32_t nowMs, bool useInitialDelay) {
  const BlinkSettings& blink = spec(targetExpression_).blink;
  blinkStartedMs_ = 0;
  if (!blink.enabled) {
    nextBlinkAtMs_ = 0;
    return;
  }

  uint32_t delayMs = blink.initialDelayMs;
  if (!useInitialDelay) {
    delayMs = blink.minIntervalMs;
    if (blink.maxIntervalMs > blink.minIntervalMs) {
      delayMs += random(0, blink.maxIntervalMs - blink.minIntervalMs + 1);
    }
  }
  nextBlinkAtMs_ = nowMs + delayMs;
}

float AvatarEngine::blinkScale(uint32_t nowMs) {
  const BlinkSettings& blink = spec(targetExpression_).blink;
  if (!blink.enabled) return 1.0f;

  if (blinkStartedMs_ == 0 && reached(nowMs, nextBlinkAtMs_)) {
    blinkStartedMs_ = nowMs;
  }
  if (blinkStartedMs_ == 0) return 1.0f;

  const uint32_t elapsed = nowMs - blinkStartedMs_;
  const uint32_t durationMs = blink.closeMs + blink.openMs;
  if (durationMs == 0 || elapsed >= durationMs) {
    scheduleNextBlink(nowMs, false);
    return 1.0f;
  }
  constexpr float kClosedScale = 0.08f;
  if (elapsed < blink.closeMs) {
    const float progress = smootherStep(
        elapsed / static_cast<float>(std::max<uint16_t>(1, blink.closeMs)));
    return 1.0f + (kClosedScale - 1.0f) * progress;
  }
  const float progress = smootherStep(
      (elapsed - blink.closeMs) /
      static_cast<float>(std::max<uint16_t>(1, blink.openMs)));
  return kClosedScale + (1.0f - kClosedScale) * progress;
}

void AvatarEngine::addEyeShapes(const EyePose& eye, float centerX,
                                float centerY, float blink) {
  const float eyeX = centerX + eye.x;
  const float eyeY = centerY + eye.y;
  const float width = std::max(4.0f, eye.width);
  const float height = std::max(4.0f, eye.height * blink);
  const float radius = std::max(
      2.0f, std::min(width, height) * 0.5f * clamp01(eye.roundness));

  // Lids and brows are drawn for an upright eye, so only plain rounded eyes
  // follow the head roll.
  const bool canRotate = fabsf(eye.angle) > 0.25f &&
                         eye.upperLid < 0.01f && eye.lowerLid < 0.01f &&
                         eye.browOpacity < 0.01f && eye.roundness > 0.92f;
  const float angle = canRotate ? eye.angle * kPi / 180.0f : 0.0f;

  // Lid cuts are expressed in the eye's local frame, centred on the eye. The
  // upper lid line runs from one side of the eye to the other, dropping by
  // upperTilt towards the side the tilt points at.
  const float upperCover = clamp01(eye.upperLid) * height;
  const float upperTilt = fabsf(eye.upperLidTilt) * height * 0.34f;
  const float upperLidY = -height * 0.5f + upperCover + upperTilt * 0.5f;
  const float upperLidSlope =
      (eye.upperLidTilt > 0.0f ? upperTilt : -upperTilt) / width;
  const float lowerCover = clamp01(eye.lowerLid) * height;
  rasterizer_.addEye(eyeX, eyeY, width * 0.5f, height * 0.5f, radius, angle,
                     upperCover > 0.0f || upperTilt > 0.0f, upperLidY,
                     upperLidSlope, lowerCover > 0.0f,
                     height * 0.5f - lowerCover, kEyeBrightness);

  const float browOpacity = clamp01(eye.browOpacity);
  if (browOpacity > 0.01f) {
    const float browLength = eye.width * 0.88f;
    const float radians = eye.browTilt * kPi / 180.0f;
    const float dx = cosf(radians) * browLength * 0.5f;
    const float dy = sinf(radians) * browLength * 0.5f;
    const float browCenterY = eyeY + eye.browY;
    const float browRadius =
        std::max(2.0f, std::min(eye.width, eye.height) * 0.04f);
    const float brightness = (160.0f + 95.0f * browOpacity) / 255.0f;
    rasterizer_.addCapsule(eyeX - dx, browCenterY - dy, eyeX + dx,
                           browCenterY + dy, browRadius, brightness);
  }
}

void AvatarEngine::addDizzyEyePattern(const EyePose& eye, float centerX,
                                      float centerY, int8_t side,
                                      uint32_t nowMs) {
  const float eyeX = centerX + eye.x;
  const float eyeY = centerY + eye.y;
  const float radius = std::min(eye.width, eye.height) * 0.5f;
  if (radius < 8.0f) return;

  // Alternating black and white discs create concentric bands. Each smaller
  // disc has a slightly different orbit, turning regular rings into an
  // off-centre vortex that remains readable on a pure-black background.
  const float time = nowMs / 1000.0f;
  const float phase = time * (3.0f + shakeIntensity_ * 1.8f) +
                      (side > 0 ? kPi : 0.0f);
  constexpr float kRadiusScale[] = {0.74f, 0.58f, 0.43f, 0.29f, 0.14f};
  constexpr float kBrightness[] = {kBackgroundBrightness, kEyeBrightness,
                                   kBackgroundBrightness, kEyeBrightness,
                                   kBackgroundBrightness};

  for (uint8_t index = 0; index < 5; ++index) {
    const float orbit = radius * (0.025f + index * 0.014f);
    const float layerPhase = phase + index * 0.82f;
    rasterizer_.addDisc(eyeX + cosf(layerPhase) * orbit,
                        eyeY + sinf(layerPhase) * orbit,
                        std::max(2.0f, radius * kRadiusScale[index]),
                        kBrightness[index]);
  }
}

void AvatarEngine::addDizzyLightning(const EyePose& eye, float centerX,
                                     float centerY, int8_t side,
                                     uint32_t nowMs) {
  const uint8_t periodSlots = shakeIntensity_ > 0.55f ? 7 : 12;
  const uint8_t sideOffset = side > 0 ? periodSlots / 2 : 0;
  const uint8_t slot = (nowMs / 85 + sideOffset) % periodSlots;
  if (slot > 1) return;

  const float eyeX = centerX + eye.x;
  const float eyeY = centerY + eye.y;
  const float radius = std::max(eye.width, eye.height) * 0.5f;
  const float direction = side < 0 ? -1.0f : 1.0f;
  const float x0 = eyeX + direction * radius * 0.72f;
  const float y0 = eyeY - radius * 0.48f;
  const float x1 = x0 + direction * 9.0f;
  const float y1 = y0 - 8.0f;
  const float x2 = x1 - direction * 4.0f;
  const float y2 = y1 - 9.0f;
  const float x3 = x2 + direction * 11.0f;
  const float y3 = y2 - 9.0f;

  // A thin stroke whose anti-aliased rim replaces the old gray side lines.
  constexpr float kBoltRadius = 1.2f;
  rasterizer_.addCapsule(x0, y0, x1, y1, kBoltRadius, kEyeBrightness);
  rasterizer_.addCapsule(x1, y1, x2, y2, kBoltRadius, kEyeBrightness);
  rasterizer_.addCapsule(x2, y2, x3, y3, kBoltRadius, kEyeBrightness);
}

AvatarEngine::DirtyRect AvatarEngine::mergeRects(const DirtyRect& first,
                                                  const DirtyRect& second) {
  if (!first.valid) return second;
  if (!second.valid) return first;
  const int left = std::min<int>(first.x, second.x);
  const int top = std::min<int>(first.y, second.y);
  const int right = std::max<int>(first.x + first.width,
                                  second.x + second.width);
  const int bottom = std::max<int>(first.y + first.height,
                                   second.y + second.height);
  return {static_cast<int16_t>(left), static_cast<int16_t>(top),
          static_cast<int16_t>(right - left),
          static_cast<int16_t>(bottom - top), true};
}

bool AvatarEngine::rectsOverlap(const DirtyRect& first,
                                const DirtyRect& second) {
  return first.valid && second.valid &&
         first.x < second.x + second.width &&
         second.x < first.x + first.width &&
         first.y < second.y + second.height &&
         second.y < first.y + first.height;
}

AvatarEngine::DirtyRect AvatarEngine::shapeBounds(uint8_t first,
                                                  uint8_t end) const {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  if (!rasterizer_.bounds(first, end, left, top, right, bottom)) return {};
  left = std::max(0, left);
  top = std::max(0, top);
  right = std::min<int>(M5.Display.width(), right);
  bottom = std::min<int>(M5.Display.height(), bottom);
  if (left >= right || top >= bottom) return {};
  return {static_cast<int16_t>(left), static_cast<int16_t>(top),
          static_cast<int16_t>(right - left),
          static_cast<int16_t>(bottom - top), true};
}

void AvatarEngine::presentRegion(const DirtyRect& rect) {
  if (!rect.valid || rect.width <= 0 || rect.height <= 0) return;
  // The AMOLED controller addresses pixels in pairs, so keep regions even.
  const int left = rect.x & ~1;
  const int top = rect.y & ~1;
  const int right =
      std::min<int>(M5.Display.width(), (rect.x + rect.width + 1) & ~1);
  const int bottom =
      std::min<int>(M5.Display.height(), (rect.y + rect.height + 1) & ~1);

  M5.Display.startWrite();
  for (int x = left; x < right; x += kBandWidth) {
    const int columns = std::min(kBandWidth, right - x);
    // Narrow regions fit more rows per band, so they need fewer copies.
    const int bandRows = kBandPixels / columns;
    for (int y = top; y < bottom; y += bandRows) {
      const int rows = std::min(bandRows, bottom - y);
      rasterizer_.render(x, y, columns, rows, bandPixels);
      M5.Display.pushImage(x, y, columns, rows,
                           reinterpret_cast<const lgfx::swap565_t*>(bandPixels));
    }
  }
  M5.Display.endWrite();
}

void AvatarEngine::render(uint32_t nowMs) {
  updateInteraction(nowMs);
  const int width = M5.Display.width();
  const int height = M5.Display.height();
  const float designScale = std::min(width, height) / kDesignSize;
  const float time = nowMs / 1000.0f;
  const ExpressionSpec& expression = spec(targetExpression_);
  const MotionSample lifeMotion = sampleMotion(kIdleMotion, time);
  const MotionSample emotionMotion = sampleMotion(expression.motion, time);

  float emotionBlend = 0.0f;
  if (targetExpression_ != ExpressionId::Idle) {
    emotionBlend = smootherStep(
        (nowMs - expressionStartedMs_) /
        static_cast<float>(std::max<uint16_t>(1, expression.motion.blendInMs)));
    if (returnToBase_ && expressionEndsAtMs_ != 0) {
      const int32_t remainingMs =
          static_cast<int32_t>(expressionEndsAtMs_ - nowMs);
      emotionBlend *= smootherStep(
          remainingMs /
          static_cast<float>(std::max<uint16_t>(1,
                                                expression.motion.blendOutMs)));
    }
  }

  const MotionSample motion =
      mixMotion(lifeMotion, emotionMotion, emotionBlend);
  Pose renderedPose = currentPose_;
  if (swipePreviewDirection_ != 0 && swipePreviewAmount_ > 0.005f) {
    const Pose& previewPose =
        spec(adjacentExpression(swipePreviewDirection_)).keyframes[0].pose;
    renderedPose = interpolate(renderedPose, previewPose,
                               swipePreviewAmount_, Easing::Smooth);
  }
  const float swipeTravel = std::min(
      1.0f, (fabsf(swipeOffsetX_) + fabsf(swipeOffsetY_) * 0.6f) / 110.0f);
  const float scale = renderedPose.faceScale * motion.scale *
                      (1.0f - swipeTravel * 0.025f) *
                      (1.0f - shakeIntensity_ * 0.018f);
  AttentionPose attention = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0};
  if (targetExpression_ == ExpressionId::Idle) {
    attention = sampleIdleAttention(nowMs);
    attention.eyeX += microSaccade(nowMs, 17.29f) * 3.8f;
    attention.eyeY += microSaccade(nowMs, 31.73f) * 2.3f;
  }

  const float gazeX = motion.eyeX + interactionX_ + shakeOffsetX_ +
                      attention.eyeX * attentionWeight_;
  const float gazeY = motion.eyeY + interactionY_ + shakeOffsetY_ +
                      attention.eyeY * attentionWeight_;
  renderedPose.leftEye.x += gazeX;
  renderedPose.leftEye.y += gazeY;
  renderedPose.rightEye.x += gazeX;
  renderedPose.rightEye.y += gazeY;

  const float headYaw = renderedPose.headYaw + headYaw_ +
                        attention.yaw * attentionWeight_ +
                        shakeOffsetX_ / kShakeTravelX * 8.0f;
  const float headPitch = renderedPose.headPitch + headPitch_ +
                          attention.pitch * attentionWeight_ +
                          swipeOffsetY_ / 110.0f * 6.0f +
                          shakeOffsetY_ / kShakeTravelY * 6.0f;
  const float headRoll = renderedPose.headRoll + headRoll_ +
                         attention.roll * attentionWeight_ -
                         swipeOffsetX_ / 120.0f * 7.0f -
                         shakeOffsetX_ / kShakeTravelX * 10.0f;
  projectEyeOntoHead(renderedPose.leftEye, -1.0f, headYaw, headPitch,
                     headRoll);
  projectEyeOntoHead(renderedPose.rightEye, 1.0f, headYaw, headPitch,
                     headRoll);

  const float eyeScale = scale * designScale;
  const auto scaleEye = [eyeScale](EyePose& eye) {
    eye.x *= eyeScale;
    eye.y *= eyeScale;
    eye.width *= eyeScale;
    eye.height *= eyeScale;
    eye.browY *= eyeScale;
  };
  scaleEye(renderedPose.leftEye);
  scaleEye(renderedPose.rightEye);

  // Positions stay fractional: the rasterizer places edges at sub-pixel
  // precision, so slow motion glides instead of stepping a pixel at a time.
  const float centerX =
      width / 2 + (renderedPose.faceX + motion.faceX) * designScale +
      swipeOffsetX_ + shakeOffsetX_ * designScale * 0.30f;
  const float centerY =
      height / 2 + (renderedPose.faceY + motion.faceY) * designScale +
      swipeOffsetY_ + shakeOffsetY_ * designScale * 0.30f;

  const float blink = blinkScale(nowMs);
  const bool dizzy = targetExpression_ == ExpressionId::Dizzy;
  rasterizer_.clear();
  addEyeShapes(renderedPose.leftEye, centerX, centerY, blink);
  if (dizzy) {
    addDizzyEyePattern(renderedPose.leftEye, centerX, centerY, -1, nowMs);
    addDizzyLightning(renderedPose.leftEye, centerX, centerY, -1, nowMs);
  }
  const uint8_t leftShapeCount = rasterizer_.shapeCount();
  addEyeShapes(renderedPose.rightEye, centerX, centerY, blink);
  if (dizzy) {
    addDizzyEyePattern(renderedPose.rightEye, centerX, centerY, 1, nowMs);
    addDizzyLightning(renderedPose.rightEye, centerX, centerY, 1, nowMs);
  }
  // Each eye's bounds are exactly where its shapes can draw this frame.
  const DirtyRect leftBounds = shapeBounds(0, leftShapeCount);
  const DirtyRect rightBounds =
      shapeBounds(leftShapeCount, rasterizer_.shapeCount());

  // Draw into the frame buffer only; update() or present() sends it to the
  // panel, while other drawing code keeps its automatic flush.
  M5.Display.setAutoDisplay(false);
  if (requiresFullClear_) {
    M5.Display.fillScreen(kBackground);
    requiresFullClear_ = false;
  }
  // Each region covers where an eye was last frame and where it is now, so
  // rendering it in full both erases the old pixels and draws the new ones.
  const DirtyRect leftRegion = mergeRects(previousLeftBounds_, leftBounds);
  const DirtyRect rightRegion = mergeRects(previousRightBounds_, rightBounds);
  if (rectsOverlap(leftRegion, rightRegion)) {
    presentRegion(mergeRects(leftRegion, rightRegion));
  } else {
    presentRegion(leftRegion);
    presentRegion(rightRegion);
  }
  M5.Display.setAutoDisplay(true);

  previousLeftBounds_ = leftBounds;
  previousRightBounds_ = rightBounds;
}

void AvatarEngine::recordRenderMetrics(uint32_t nowMs, uint32_t frameWorkUs,
                                       uint32_t frameStartedUs) {
  totalRenderTimeUs_ += frameWorkUs;
  maximumRenderTimeUs_ = std::max(maximumRenderTimeUs_, frameWorkUs);
  ++metricsFrameCount_;

  if (previousRenderStartedUs_ != 0) {
    const uint32_t intervalUs = frameStartedUs - previousRenderStartedUs_;
    totalFrameIntervalUs_ += intervalUs;
    maximumFrameIntervalUs_ = std::max(maximumFrameIntervalUs_, intervalUs);
  }
  previousRenderStartedUs_ = frameStartedUs;

  const uint32_t windowMs = nowMs - metricsStartedMs_;
  if (windowMs < kMetricsReportIntervalMs || metricsFrameCount_ == 0) return;

  const float fps = metricsFrameCount_ * 1000.0f / windowMs;
  const float averageRenderMs =
      totalRenderTimeUs_ / (metricsFrameCount_ * 1000.0f);
  const uint32_t intervalCount = metricsFrameCount_ > 1 ? metricsFrameCount_ - 1 : 1;
  const float averageIntervalMs =
      totalFrameIntervalUs_ / (intervalCount * 1000.0f);
  Serial.printf(
      "[avatar perf] fps=%.1f render_avg=%.2fms render_max=%.2fms "
      "frame_avg=%.2fms frame_max=%.2fms\n",
      fps, averageRenderMs, maximumRenderTimeUs_ / 1000.0f,
      averageIntervalMs, maximumFrameIntervalUs_ / 1000.0f);

  metricsStartedMs_ = nowMs;
  metricsFrameCount_ = 0;
  totalRenderTimeUs_ = 0;
  maximumRenderTimeUs_ = 0;
  totalFrameIntervalUs_ = 0;
  maximumFrameIntervalUs_ = 0;
  previousRenderStartedUs_ = 0;
}

void AvatarEngine::update(uint32_t nowMs) {
  if (!ready_) return;

  const uint32_t elapsed = nowMs - transitionStartedMs_;
  if (transitionDurationMs_ > 0 && elapsed < transitionDurationMs_) {
    currentPose_ = interpolate(fromPose_, targetPose_,
                               elapsed / static_cast<float>(transitionDurationMs_),
                               transitionEasing_);
  } else {
    currentPose_ = targetPose_;
  }

  advanceTimeline(nowMs);

  const uint32_t nowUs = micros();
  if (presentOnRefresh_) {
    if (framePending_) return;
  } else {
    const bool frameDue =
        nextFrameUs_ == 0 || static_cast<int32_t>(nowUs - nextFrameUs_) >= 0;
    if (!forceRender_ && !frameDue) return;

    if (forceRender_ || nextFrameUs_ == 0) {
      nextFrameUs_ = nowUs + kFrameIntervalUs;
    } else {
      nextFrameUs_ += kFrameIntervalUs;
      if (static_cast<int32_t>(nowUs - nextFrameUs_) >=
          static_cast<int32_t>(kFrameIntervalUs * 2)) {
        nextFrameUs_ = nowUs + kFrameIntervalUs;
      }
    }
  }
  forceRender_ = false;

  const uint32_t renderStartedUs = micros();
  render(nowMs);
  if (presentOnRefresh_) {
    framePending_ = true;
    pendingRenderUs_ = micros() - renderStartedUs;
    return;
  }
  M5.Display.display();
  recordRenderMetrics(nowMs, micros() - renderStartedUs, renderStartedUs);
}

void AvatarEngine::present() {
  if (!framePending_) return;
  const uint32_t presentStartedUs = micros();
  M5.Display.display();
  framePending_ = false;
  recordRenderMetrics(millis(),
                      pendingRenderUs_ + (micros() - presentStartedUs),
                      presentStartedUs);
}
