// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <stdint.h>

// Anti-aliased scanline renderer for the avatar's grayscale shapes.
//
// Every shape is a signed distance function sampled at pixel centres, so edges
// receive fractional coverage at sub-pixel positions instead of the integer
// staircase produced by solid fill primitives. Shapes are composited in
// painter's order: solid spans are filled with a precomputed panel color and
// only the few edge pixels per row are blended in linear light. Output is
// byte-swapped RGB565, the panel's native format, so a rendered band can be
// copied to the display without per-pixel conversion.
//
// Coordinates are continuous screen pixels: pixel (x, y) covers the square
// from (x, y) to (x + 1, y + 1) and is sampled at its centre.
class EyeRasterizer {
 public:
  static constexpr uint8_t kMaxShapes = 24;

  EyeRasterizer();

  void clear() { shapeCount_ = 0; }

  // Rounded rectangle centred on (centerX, centerY), rotated clockwise by
  // angleRadians. Lid cuts are straight lines in the eye's local frame
  // (origin at the centre, y pointing down): the upper lid keeps points below
  // y = upperLidY + upperLidSlope * x and the lower lid keeps points above
  // y = lowerLidY. Brightness is a display gray level from 0 to 1.
  void addEye(float centerX, float centerY, float halfWidth, float halfHeight,
              float cornerRadius, float angleRadians, bool upperLid,
              float upperLidY, float upperLidSlope, bool lowerLid,
              float lowerLidY, float brightness);

  // Line segment with round caps; a zero-length segment is a disc.
  void addCapsule(float startX, float startY, float endX, float endY,
                  float radius, float brightness);
  void addDisc(float centerX, float centerY, float radius, float brightness) {
    addCapsule(centerX, centerY, centerX, centerY, radius, brightness);
  }

  uint8_t shapeCount() const { return shapeCount_; }

  // Pixel rectangle (right and bottom exclusive) containing every pixel that
  // shapes [first, end) can touch. Returns false for an empty range.
  bool bounds(uint8_t first, uint8_t end, int& left, int& top, int& right,
              int& bottom) const;

  // Renders `rows` rows of `width` pixels starting at (x, y) into `out`, row
  // after row. Uncovered pixels are black.
  void render(int x, int y, int width, int rows, uint16_t* out);

 private:
  enum class Kind : uint8_t { Eye, Capsule };

  struct Shape {
    Kind kind;
    bool upperLid;
    bool lowerLid;
    uint16_t level;  // Linear-light intensity, for blending edges.
    uint16_t color;  // Panel pixel for fully covered pixels.
    // Bounds outside which coverage is always zero: rows by pixel centre,
    // columns as an inclusive pixel range.
    float top;
    float bottom;
    int firstColumn;
    int lastColumn;
    // Eye: centre and unit x axis of the rotated local frame.
    // Capsule: start point and start-to-end vector.
    float originX;
    float originY;
    float directionX;
    float directionY;
    float radius;
    // Eye only: rounded-box half extents excluding the corner radius.
    float innerHalfWidth;
    float innerHalfHeight;
    float upperLidY;
    float upperLidSlope;
    float upperLidScale;
    float lowerLidY;
    // Capsule only: 1 / |start-to-end vector|^2, or 0 for a disc.
    float inverseLengthSquared;
  };

  Shape* nextShape(float brightness);
  static void setColumns(Shape* shape, float left, float right);
  float distance(const Shape& shape, float sampleX, float sampleY) const;
  // Steps `column` towards `limit` (inclusive) until it reaches a pixel with
  // any coverage, whose distance is returned in `edgeDistance`.
  bool seekCoverage(const Shape& shape, float sampleY, int direction,
                    int limit, int& column, float& edgeDistance) const;
  void renderRow(int x, int y, int width, uint16_t* out);

  Shape shapes_[kMaxShapes];
  uint8_t shapeCount_ = 0;
};
