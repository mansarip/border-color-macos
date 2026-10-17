// Border engine: draws a coloured border around every normal application window and
// keeps it attached while the window moves, resizes, hides, or changes Space.
//
// Call everything on the main thread; the engine runs on the main run loop.
#pragma once
#include <stdbool.h>
#include <sys/types.h>

typedef struct {
  double r, g, b, a;  // sRGB, 0...1
} be_color;

// Asked once per window (and again after be_refresh_colors). Return true and fill
// *color if windows owned by `pid` should get a border.
typedef bool (*be_color_fn)(pid_t pid, be_color *color, void *context);

// Starts tracking windows. Returns false when a required private API is missing on
// this macOS version; be_missing_symbol() then names it.
bool be_start(be_color_fn color_fn, void *context);

void be_set_enabled(bool enabled);

// Border thickness in points (clamped to 1...40). May be called before be_start.
void be_set_width(double width);

// Outer corner radius of every border in points; 0 gives square corners. Negative (the
// default): follow each window's own corner. May be called before be_start.
void be_set_corner_radius(double radius);

// When true (the default), borders of unfocused windows are drawn at half opacity;
// when false, every border is solid. May be called before be_start.
void be_set_dim_inactive(bool dim);

// Re-asks color_fn for every tracked window and redraws borders whose colour changed.
void be_refresh_colors(void);

const char *be_missing_symbol(void);
