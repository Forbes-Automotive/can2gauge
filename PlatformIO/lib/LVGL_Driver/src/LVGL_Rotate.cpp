// 0.25.1: Screen rotation - the pixel and touch maths (see LVGL_Rotate.h for how it fits together)
#include "LVGL_Rotate.h"

#if SCREEN_ROTATION
#include <Arduino.h>
#include <math.h>
#include "Display_ST7701.h" // DISPLAY_WIDTH / DISPLAY_HEIGHT

#define ROTATE_CENTRE_X (DISPLAY_WIDTH / 2.0f)  // the middle of the glass, where the picture turns round (240)
#define ROTATE_CENTRE_Y (DISPLAY_HEIGHT / 2.0f) // (240)
#define FIXED_ONE 65536.0f                      // whole-number maths for the pixel loop: 1 pixel = 65536 (16 bits of fraction)

static float angleCos = 1.0f; // the turn as cosine / sine (worked out once in rotateSetAngle)
static float angleSin = 0.0f;
static int32_t stepX = 0;     // moving one pixel right on the glass moves this far across the canvas (fixed point) ...
static int32_t stepY = 0;     // ... and this far down it
// the round glass: only the pixels inside the circle are seen, so only those are turned (about 1/5 less work)
static uint16_t rowFirst[DISPLAY_HEIGHT]; // first pixel of each row that's inside the circle
static uint16_t rowLast[DISPLAY_HEIGHT];  // ... and the last
static bool rowsWorkedOut = false;        // rowFirst / rowLast filled in

// which pixels of each row are inside the circle (done once)
static void workOutRows()
{
  const float radius = DISPLAY_WIDTH / 2.0f;
  for (uint16_t row = 0; row < DISPLAY_HEIGHT; row++)
  {
    const float fromMiddle = row + 0.5f - ROTATE_CENTRE_Y; // this row's height above / below the middle
    float halfWidth = 0.0f;
    if (fabsf(fromMiddle) < radius)
    {
      halfWidth = sqrtf(radius * radius - fromMiddle * fromMiddle) + 1.0f; // +1: a pixel extra, the edge is never short
    }
    int32_t first = (int32_t)(ROTATE_CENTRE_X - halfWidth);
    int32_t last = (int32_t)(ROTATE_CENTRE_X + halfWidth);
    if (first < 0)
    {
      first = 0;
    }
    if (last > DISPLAY_WIDTH - 1)
    {
      last = DISPLAY_WIDTH - 1;
    }
    rowFirst[row] = (uint16_t)first;
    rowLast[row] = (uint16_t)last;
  }
  rowsWorkedOut = true;
}

void rotateSetAngle(float degrees)
{
  if (!rowsWorkedOut)
  {
    workOutRows();
  }
  const float radians = degrees * (float)M_PI / 180.0f;
  angleCos = cosf(radians);
  angleSin = sinf(radians);
  // each pixel of the glass looks up the canvas pixel that lands on it: the glass position turned BACK by the angle
  stepX = (int32_t)lroundf(angleCos * FIXED_ONE);
  stepY = (int32_t)lroundf(-angleSin * FIXED_ONE);
}

// The turned copy. For every pixel of the glass (inside the circle), the canvas pixel that the turn brings there is
// copied (the nearest one - no blending, which keeps it fast). Along a row the canvas position moves by the same
// step each pixel, so the loop only adds.
// optimize("O3"): this one loop is compiled for speed (the rest of the gauge is compiled for size) - it runs for every
// pixel of every frame while a rotation is set
__attribute__((optimize("O3"))) void rotateFrame(const uint16_t *canvas, uint16_t *glass)
{
  for (uint16_t row = 0; row < DISPLAY_HEIGHT; row++)
  {
    const uint16_t first = rowFirst[row];
    const uint16_t last = rowLast[row];
    // where the row's first pixel comes from on the canvas (the middle of the pixel, turned back round the centre)
    const float glassX = first + 0.5f - ROTATE_CENTRE_X;
    const float glassY = row + 0.5f - ROTATE_CENTRE_Y;
    int32_t canvasX = (int32_t)((angleCos * glassX + angleSin * glassY + ROTATE_CENTRE_X) * FIXED_ONE);
    int32_t canvasY = (int32_t)((-angleSin * glassX + angleCos * glassY + ROTATE_CENTRE_Y) * FIXED_ONE);
    uint16_t *out = glass + row * DISPLAY_WIDTH;
    for (uint16_t column = first; column <= last; column++)
    {
      const int32_t pixelX = canvasX >> 16; // the whole-pixel part (rounds down, also below zero)
      const int32_t pixelY = canvasY >> 16;
      if (pixelX >= 0 && pixelX < DISPLAY_WIDTH && pixelY >= 0 && pixelY < DISPLAY_HEIGHT)
      {
        out[column] = canvas[pixelY * DISPLAY_WIDTH + pixelX];
      }
      else
      {
        out[column] = 0; // off the edge of the canvas: black
      }
      canvasX += stepX;
      canvasY += stepY;
    }
  }
}

// a touch on the glass -> the same point on the canvas (what LVGL drew under the finger)
void rotateTouch(int32_t &x, int32_t &y)
{
  const float glassX = x + 0.5f - ROTATE_CENTRE_X;
  const float glassY = y + 0.5f - ROTATE_CENTRE_Y;
  int32_t canvasX = (int32_t)floorf(angleCos * glassX + angleSin * glassY + ROTATE_CENTRE_X);
  int32_t canvasY = (int32_t)floorf(-angleSin * glassX + angleCos * glassY + ROTATE_CENTRE_Y);
  if (canvasX < 0)
  {
    canvasX = 0;
  }
  if (canvasX > DISPLAY_WIDTH - 1)
  {
    canvasX = DISPLAY_WIDTH - 1;
  }
  if (canvasY < 0)
  {
    canvasY = 0;
  }
  if (canvasY > DISPLAY_HEIGHT - 1)
  {
    canvasY = DISPLAY_HEIGHT - 1;
  }
  x = canvasX;
  y = canvasY;
}

#endif
