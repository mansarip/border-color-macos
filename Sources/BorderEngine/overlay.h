// A border drawn as up to four thin windows around a target window: top and bottom
// strips span the full width and contain the rounded corners, left and right strips
// cover the straight edges in between. Strips keep backing stores small; one
// transparent window covering a large Retina window costs ~38 MB, four strips ~3 MB.
#pragma once
#include "border_engine.h"
#include <CoreGraphics/CoreGraphics.h>

#define OVERLAY_MAX_STRIPS 4

typedef struct {
  float width;         // border thickness in points
  float radius;        // target window's corner radius
  float outer_radius;  // border's outer corner radius; negative: radius + width
  be_color color;
  float alpha;         // whole-border opacity (focus state)
  uint64_t space;      // Space the target lives on
  int level;           // target window level
  bool expose;         // drawn over a Mission Control thumbnail: no target, no Space, not faded
} overlay_style;

typedef struct {
  int count;
  uint32_t wid[OVERLAY_MAX_STRIPS];
  CGContextRef context[OVERLAY_MAX_STRIPS];
  CGRect rect[OVERLAY_MAX_STRIPS];  // global, top-left origin
  CGRect frame;                     // target frame the strips were laid out for
  overlay_style style;
  bool shown;
} overlay;

// Shows or updates the border around `target`, whose frame is `frame` (global coordinates).
// With style->expose, `target` is ignored and the border sits on top of its level.
void overlay_show(overlay *o, int cid, uint32_t target, CGRect frame, const overlay_style *style);

// Follows a target that moved; only repositions the strips when its size is unchanged.
void overlay_move(overlay *o, int cid, uint32_t target, CGRect frame);

// Puts the strips back directly below `target` after it changed position in the stack.
void overlay_restack(overlay *o, int cid, uint32_t target);

void overlay_set_alpha(overlay *o, int cid, float alpha);
void overlay_hide(overlay *o, int cid);
void overlay_destroy(overlay *o, int cid);
