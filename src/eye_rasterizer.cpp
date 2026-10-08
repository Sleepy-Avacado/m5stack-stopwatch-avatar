// SPDX-License-Identifier: AGPL-3.0-or-later

// The Arduino and PlatformIO ESP32 builds default to -Os, which leaves the
// span fills here about 3x slower than -O2 on the ESP32-S3.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "eye_rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Inlined into the scanline loops: with the ESP32-S3's windowed call ABI the
// call overhead would otherwise outweigh the distance arithmetic itself.
#define EYE_RASTER_INLINE inline __attribute__((always_inline))

namespace {

constexpr int kMaxLevel = 4095;
// Coverage is mixed in linear light and encoded for a display gamma of about
// 2.2, so a half-covered edge pixel emits half the light of a covered one and
// edges keep their true position instead of looking thin and stepped.
constexpr float kDisplayGamma = 2.2f;

// Linear light level to byte-swapped RGB565 gray, and back from the 6-bit
// green channel of such a pixel.
uint16_t encodedLevels[kMaxLevel + 1];
uint16_t decodedGreens[64];
bool tablesReady = false;

void buildTables() {
  if (tablesReady) return;
  for (int level = 0; level <= kMaxLevel; ++level) {
    const float gray =
        powf(level / static_cast<float>(kMaxLevel), 1.0f / kDisplayGamma);
    const uint16_t redBlue = static_cast<uint16_t>(lroundf(gray * 31.0f));
    const uint16_t green = static_cast<uint16_t>(lroundf(gray * 63.0f));
    const uint16_t rgb565 = (redBlue << 11) | (green << 5) | redBlue;
    encodedLevels[level] = static_cast<uint16_t>((rgb565 >> 8) | (rgb565 << 8));
  }
  for (int green = 0; green < 64; ++green) {
    decodedGreens[green] = static_cast<uint16_t>(
        lroundf(powf(green / 63.0f, kDisplayGamma) * kMaxLevel));
  }
  tablesReady = true;
}

uint16_t linearLevel(float brightness) {
  const float gray = std::max(0.0f, std::min(1.0f, brightness));
  return static_cast<uint16_t>(lroundf(powf(gray, kDisplayGamma) * kMaxLevel));
}

// The ESP32-S3 FPU has no square root instruction and sqrtf is a slow library
// call, so distances use a bit-level reciprocal estimate refined by two Newton
// steps. The relative error stays below 1e-5, about 0.002 px on this screen.
EYE_RASTER_INLINE float fastSqrt(float value) {
  if (value <= 0.0f) return 0.0f;
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  bits = 0x5f375a86u - (bits >> 1);
  float inverse;
  memcpy(&inverse, &bits, sizeof(inverse));
  inverse *= 1.5f - 0.5f * value * inverse * inverse;
  inverse *= 1.5f - 0.5f * value * inverse * inverse;
  return value * inverse;
}

// Edge pixels blend over whatever earlier shapes left in the row. Rows hold
// finished panel pixels, so the light level underneath is decoded from the
// green channel, which is precise enough for the rare overlapping edge.
EYE_RASTER_INLINE void blendPixel(uint16_t& pixel, int target,
                                  float coverage) {
  const uint16_t rgb565 = static_cast<uint16_t>((pixel >> 8) | (pixel << 8));
  const int current = decodedGreens[(rgb565 >> 5) & 0x3F];
  pixel = encodedLevels[current +
                        static_cast<int>((target - current) * coverage)];
}

}  // namespace

EyeRasterizer::EyeRasterizer() { buildTables(); }

EyeRasterizer::Shape* EyeRasterizer::nextShape(float brightness) {
  if (shapeCount_ >= kMaxShapes) return nullptr;
  Shape* shape = &shapes_[shapeCount_++];
  *shape = Shape{};
  shape->level = linearLevel(brightness);
  shape->color = encodedLevels[shape->level];
  return shape;
}

void EyeRasterizer::setColumns(Shape* shape, float left, float right) {
  shape->firstColumn = static_cast<int>(floorf(left));
  shape->lastColumn = static_cast<int>(ceilf(right)) - 1;
}

void EyeRasterizer::addEye(float centerX, float centerY, float halfWidth,
                           float halfHeight, float cornerRadius,
                           float angleRadians, bool upperLid, float upperLidY,
                           float upperLidSlope, bool lowerLid, float lowerLidY,
                           float brightness) {
  Shape* shape = nextShape(brightness);
  if (shape == nullptr) return;
  const float radius = std::max(
      0.0f, std::min(cornerRadius, std::min(halfWidth, halfHeight)));
  shape->kind = Kind::Eye;
  shape->originX = centerX;
  shape->originY = centerY;
  shape->directionX = cosf(angleRadians);
  shape->directionY = sinf(angleRadians);
  shape->radius = radius;
  shape->innerHalfWidth = halfWidth - radius;
  shape->innerHalfHeight = halfHeight - radius;
  shape->upperLid = upperLid;
  shape->upperLidY = upperLidY;
  shape->upperLidSlope = upperLidSlope;
  shape->upperLidScale = 1.0f / sqrtf(1.0f + upperLidSlope * upperLidSlope);
  shape->lowerLid = lowerLid;
  shape->lowerLidY = lowerLidY;

  const float extentX = fabsf(shape->directionX) * halfWidth +
                        fabsf(shape->directionY) * halfHeight + 1.0f;
  const float extentY = fabsf(shape->directionY) * halfWidth +
                        fabsf(shape->directionX) * halfHeight + 1.0f;
  shape->top = centerY - extentY;
  shape->bottom = centerY + extentY;
  setColumns(shape, centerX - extentX, centerX + extentX);
}

void EyeRasterizer::addCapsule(float startX, float startY, float endX,
                               float endY, float radius, float brightness) {
  Shape* shape = nextShape(brightness);
  if (shape == nullptr) return;
  shape->kind = Kind::Capsule;
  shape->originX = startX;
  shape->originY = startY;
  shape->directionX = endX - startX;
  shape->directionY = endY - startY;
  shape->radius = radius;
  const float lengthSquared = shape->directionX * shape->directionX +
                              shape->directionY * shape->directionY;
  shape->inverseLengthSquared =
      lengthSquared > 1e-6f ? 1.0f / lengthSquared : 0.0f;

  const float extent = radius + 1.0f;
  shape->top = std::min(startY, endY) - extent;
  shape->bottom = std::max(startY, endY) + extent;
  setColumns(shape, std::min(startX, endX) - extent,
             std::max(startX, endX) + extent);
}

bool EyeRasterizer::bounds(uint8_t first, uint8_t end, int& left, int& top,
                           int& right, int& bottom) const {
  end = std::min(end, shapeCount_);
  if (first >= end) return false;
  left = top = INT16_MAX;
  right = bottom = INT16_MIN;
  for (uint8_t index = first; index < end; ++index) {
    const Shape& shape = shapes_[index];
    left = std::min(left, shape.firstColumn);
    right = std::max(right, shape.lastColumn + 1);
    top = std::min(top, static_cast<int>(floorf(shape.top)));
    bottom = std::max(bottom, static_cast<int>(ceilf(shape.bottom)));
  }
  return true;
}

EYE_RASTER_INLINE float EyeRasterizer::distance(const Shape& shape,
                                                float sampleX,
                                                float sampleY) const {
  const float offsetX = sampleX - shape.originX;
  const float offsetY = sampleY - shape.originY;
  if (shape.kind == Kind::Capsule) {
    const float along = std::max(
        0.0f, std::min(1.0f, (offsetX * shape.directionX +
                              offsetY * shape.directionY) *
                                 shape.inverseLengthSquared));
    const float nearestX = offsetX - shape.directionX * along;
    const float nearestY = offsetY - shape.directionY * along;
    return fastSqrt(nearestX * nearestX + nearestY * nearestY) - shape.radius;
  }

  const float localX = offsetX * shape.directionX + offsetY * shape.directionY;
  const float localY = offsetY * shape.directionX - offsetX * shape.directionY;
  const float outsideX = fabsf(localX) - shape.innerHalfWidth;
  const float outsideY = fabsf(localY) - shape.innerHalfHeight;
  float result =
      outsideX > 0.0f && outsideY > 0.0f
          ? fastSqrt(outsideX * outsideX + outsideY * outsideY) - shape.radius
          : std::max(outsideX, outsideY) - shape.radius;
  if (shape.upperLid) {
    result = std::max(result, (shape.upperLidY +
                               shape.upperLidSlope * localX - localY) *
                                  shape.upperLidScale);
  }
  if (shape.lowerLid) {
    result = std::max(result, localY - shape.lowerLidY);
  }
  return result;
}

EYE_RASTER_INLINE bool EyeRasterizer::seekCoverage(const Shape& shape,
                                                   float sampleY,
                                                   int direction,
                                                   int limit, int& column,
                                                   float& edgeDistance) const {
  // A distance changes by at most one per pixel travelled, so a sample d
  // pixels outside the shape proves the next d - 0.5 pixels are empty too.
  while (direction > 0 ? column <= limit : column >= limit) {
    edgeDistance = distance(shape, column + 0.5f, sampleY);
    if (edgeDistance < 0.5f) return true;
    column += direction * (static_cast<int>(edgeDistance - 0.5f) + 1);
  }
  return false;
}

void EyeRasterizer::render(int x, int y, int width, int rows, uint16_t* out) {
  if (width <= 0) return;
  for (int row = 0; row < rows; ++row) {
    renderRow(x, y + row, width, out + row * width);
  }
}

void EyeRasterizer::renderRow(int x, int y, int width, uint16_t* out) {
  memset(out, 0, width * sizeof(out[0]));
  const float sampleY = y + 0.5f;
  const int end = x + width;

  for (uint8_t index = 0; index < shapeCount_; ++index) {
    const Shape& shape = shapes_[index];
    if (sampleY <= shape.top || sampleY >= shape.bottom) continue;
    const int first = std::max(x, shape.firstColumn);
    const int last = std::min(end - 1, shape.lastColumn);

    // Every shape is convex, so a row crosses it in one span: a partially
    // covered left edge, a solid middle and a partially covered right edge.
    // Search inwards from both ends for the edges, then fill the middle.
    int column = first;
    float edgeDistance = 0.0f;
    if (!seekCoverage(shape, sampleY, 1, last, column, edgeDistance)) continue;
    while (edgeDistance > -0.5f) {
      blendPixel(out[column - x], shape.level, 0.5f - edgeDistance);
      if (++column > last) break;
      edgeDistance = distance(shape, column + 0.5f, sampleY);
      if (edgeDistance >= 0.5f) {
        // Coverage ended before any pixel was solid: the span is complete.
        column = last + 1;
        break;
      }
    }
    if (column > last) continue;

    // `column` is solid, so the search from the right stops at or after it.
    const int solidStart = column;
    column = last;
    seekCoverage(shape, sampleY, -1, solidStart, column, edgeDistance);
    while (edgeDistance > -0.5f) {
      blendPixel(out[column - x], shape.level, 0.5f - edgeDistance);
      --column;
      edgeDistance = distance(shape, column + 0.5f, sampleY);
    }
    std::fill(out + (solidStart - x), out + (column + 1 - x), shape.color);
  }
}
