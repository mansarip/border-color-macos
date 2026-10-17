#include "overlay.h"
#include "skylight.h"
#include <math.h>
#include <string.h>

#define AA_MARGIN 1.0      // transparent margin so the outer edge can anti-alias
#define BACKING_SCALE 2.0  // render at Retina resolution

// Classic CGS window tags.
#define TAG_EXPOSE_FADE (1ULL << 1)    // fade out when Mission Control activates
#define TAG_IGNORES_MOUSE (1ULL << 9)  // clicks fall through to the window underneath

enum { ORDER_OUT = 0, ORDER_BELOW = -1, ORDER_ABOVE = 1 };

static void add_rounded_rect(CGMutablePathRef path, CGRect rect, CGFloat radius) {
  radius = fmin(radius, fmin(rect.size.width, rect.size.height) / 2);
  if (radius > 0) CGPathAddRoundedRect(path, NULL, rect, radius, radius);
  else CGPathAddRect(path, NULL, rect);
}

static int layout(CGRect frame, CGFloat width, CGFloat radius, CGRect strips[OVERLAY_MAX_STRIPS]) {
  CGFloat outset = width + AA_MARGIN;
  CGRect box = CGRectIntegral(CGRectInset(frame, -outset, -outset));
  // Top/bottom must reach past the end of the corner arcs (frame edge + radius); the
  // sides must reach 1pt inside the window, where the ring's inner edge sits.
  CGFloat top = ceil(CGRectGetMinY(frame) - CGRectGetMinY(box) + radius) + 1;
  CGFloat bottom = ceil(CGRectGetMaxY(box) - CGRectGetMaxY(frame) + radius) + 1;
  CGFloat left = ceil(CGRectGetMinX(frame) - CGRectGetMinX(box)) + 2;
  CGFloat right = ceil(CGRectGetMaxX(box) - CGRectGetMaxX(frame)) + 2;
  CGFloat middle = box.size.height - top - bottom;
  if (middle < 1) {
    strips[0] = box;
    return 1;
  }
  strips[0] = CGRectMake(box.origin.x, box.origin.y, box.size.width, top);
  strips[1] = CGRectMake(box.origin.x, CGRectGetMaxY(box) - bottom, box.size.width, bottom);
  strips[2] = CGRectMake(box.origin.x, box.origin.y + top, left, middle);
  strips[3] = CGRectMake(CGRectGetMaxX(box) - right, box.origin.y + top, right, middle);
  return 4;
}

static CFDictionaryRef no_shadow(void) {
  static CFDictionaryRef properties;
  if (!properties) {
    int zero = 0;
    CFNumberRef density = CFNumberCreate(NULL, kCFNumberIntType, &zero);
    const void *keys[] = {CFSTR("com.apple.WindowShadowDensity")};
    const void *values[] = {density};
    properties = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                    &kCFTypeDictionaryValueCallBacks);
    CFRelease(density);
  }
  return properties;
}

static uint32_t create_window(int cid, CGRect rect, int level, bool expose) {
  CGRect local = {CGPointZero, rect.size};
  CFTypeRef region = NULL;
  if (sl.NewRegionWithRect(&local, &region) != kCGErrorSuccess || !region) return 0;
  uint32_t wid = 0;
  sl.NewWindow(cid, kCGBackingStoreBuffered, rect.origin.x, rect.origin.y, region, &wid);
  CFRelease(region);
  if (!wid) return 0;

  // Ordinary borders fade out in Mission Control; their Exposé stand-ins must not.
  uint64_t tags = expose ? TAG_IGNORES_MOUSE : TAG_EXPOSE_FADE | TAG_IGNORES_MOUSE;
  sl.SetWindowTags(cid, wid, &tags, 64);
  sl.SetWindowResolution(cid, wid, BACKING_SCALE);
  sl.SetWindowOpacity(cid, wid, false);
  sl.SetWindowLevel(cid, wid, level);
  if (sl.WindowSetShadowProperties) sl.WindowSetShadowProperties(wid, no_shadow());
  return wid;
}

static void destroy_strips(overlay *o, int cid) {
  for (int i = 0; i < o->count; i++) {
    if (o->context[i]) CGContextRelease(o->context[i]);
    if (o->wid[i]) sl.ReleaseWindow(cid, o->wid[i]);
  }
  memset(o, 0, sizeof *o);
}

static bool create_strips(overlay *o, int cid, const CGRect *strips, int count, int level,
                          bool expose) {
  for (int i = 0; i < count; i++) {
    o->count = i + 1;
    o->rect[i] = strips[i];
    o->wid[i] = create_window(cid, strips[i], level, expose);
    if (!o->wid[i]) return false;
    o->context[i] = sl.WindowContextCreate(cid, o->wid[i], NULL);
    if (!o->context[i]) return false;
  }
  o->style.level = level;
  o->style.expose = expose;
  return true;
}

static void draw_strip(overlay *o, int cid, int i) {
  CGContextRef c = o->context[i];
  CGRect r = o->rect[i];
  CGContextSaveGState(c);
  CGContextClearRect(c, (CGRect){CGPointZero, r.size});
  // Draw in global top-left coordinates; the window context has a bottom-left origin.
  CGContextTranslateCTM(c, -r.origin.x, CGRectGetMaxY(r));
  CGContextScaleCTM(c, 1, -1);

  // A ring hugging the window: outside the frame by the border width, and 1pt inside it
  // (hidden under the window, which sits above) so no gap shows at the edge. By default
  // the outer corner follows the window's, keeping the ring as thick at the corners as
  // along the edges.
  CGFloat width = o->style.width;
  CGFloat outer = o->style.outer_radius >= 0 ? o->style.outer_radius : o->style.radius + width;
  CGMutablePathRef path = CGPathCreateMutable();
  add_rounded_rect(path, CGRectInset(o->frame, -width, -width), outer);
  add_rounded_rect(path, CGRectInset(o->frame, 1, 1), fmax(o->style.radius - 1, 0));
  CGContextAddPath(c, path);
  CGColorRef color = CGColorCreateSRGB(o->style.color.r, o->style.color.g, o->style.color.b,
                                       o->style.color.a);
  CGContextSetFillColorWithColor(c, color);
  CGContextEOFillPath(c);
  CGColorRelease(color);
  CGPathRelease(path);

  CGContextRestoreGState(c);
  CGContextFlush(c);
  sl.FlushWindowContentRegion(cid, o->wid[i], NULL);
}

static void move_to_space(overlay *o, int cid, uint64_t space) {
  CFNumberRef numbers[OVERLAY_MAX_STRIPS];
  for (int i = 0; i < o->count; i++) numbers[i] = CFNumberCreate(NULL, kCFNumberSInt32Type, &o->wid[i]);
  CFArrayRef list = CFArrayCreate(NULL, (const void **)numbers, o->count, &kCFTypeArrayCallBacks);
  sl.MoveWindowsToManagedSpace(cid, list, space);
  CFRelease(list);
  for (int i = 0; i < o->count; i++) CFRelease(numbers[i]);
}

static bool same_color(be_color a, be_color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static void update(overlay *o, int cid, uint32_t target, CGRect frame, overlay_style style,
                   bool restack) {
  CGRect strips[OVERLAY_MAX_STRIPS];
  int count = layout(frame, style.width, style.radius, strips);

  bool created = false;
  if (count != o->count) {
    destroy_strips(o, cid);
    if (!create_strips(o, cid, strips, count, style.level, style.expose)) {
      destroy_strips(o, cid);
      return;
    }
    created = true;
  }

  bool resized = false;
  for (int i = 0; i < count; i++)
    if (!CGSizeEqualToSize(strips[i].size, o->rect[i].size)) resized = true;
  // Redraw when the ring's position inside the strips changes, e.g. on resize.
  CGPoint offset = {frame.origin.x - strips[0].origin.x, frame.origin.y - strips[0].origin.y};
  CGPoint old_offset = {o->frame.origin.x - o->rect[0].origin.x,
                        o->frame.origin.y - o->rect[0].origin.y};
  bool redraw = created || resized || !CGSizeEqualToSize(frame.size, o->frame.size) ||
                !CGPointEqualToPoint(offset, old_offset) || style.width != o->style.width ||
                style.radius != o->style.radius || style.outer_radius != o->style.outer_radius ||
                !same_color(style.color, o->style.color);

  // Freeze screen updates so reshaped strips never show stale content.
  if (resized) sl.DisableUpdate(cid);
  for (int i = 0; i < count; i++) {
    if (CGSizeEqualToSize(strips[i].size, o->rect[i].size)) continue;
    CGRect local = {CGPointZero, strips[i].size};
    CFTypeRef region = NULL;
    sl.NewRegionWithRect(&local, &region);
    if (region) {
      sl.SetWindowShape(cid, o->wid[i], strips[i].origin.x, strips[i].origin.y, region);
      CFRelease(region);
    }
  }

  o->frame = frame;
  memcpy(o->rect, strips, sizeof(CGRect) * count);
  o->style.width = style.width;
  o->style.radius = style.radius;
  o->style.outer_radius = style.outer_radius;
  o->style.color = style.color;
  if (redraw)
    for (int i = 0; i < count; i++) draw_strip(o, cid, i);

  if (style.space && style.space != o->style.space) {
    move_to_space(o, cid, style.space);
    o->style.space = style.space;
  }
  if (style.level != o->style.level) {
    for (int i = 0; i < count; i++) sl.SetWindowLevel(cid, o->wid[i], style.level);
    o->style.level = style.level;
  }

  CFTypeRef transaction = sl.TransactionCreate(cid);
  if (transaction) {
    for (int i = 0; i < count; i++) {
      sl.TransactionMoveWindowWithGroup(transaction, o->wid[i], strips[i].origin);
      if (o->style.expose) {
        if (!o->shown) sl.TransactionOrderWindow(transaction, o->wid[i], ORDER_ABOVE, 0);
      } else if (restack || !o->shown) {
        sl.TransactionOrderWindow(transaction, o->wid[i], ORDER_BELOW, target);
      }
      if (!o->shown || style.alpha != o->style.alpha)
        sl.TransactionSetWindowAlpha(transaction, o->wid[i], style.alpha);
    }
    sl.TransactionCommit(transaction, 0);
    CFRelease(transaction);
  }
  o->style.alpha = style.alpha;
  o->shown = true;

  if (resized) sl.ReenableUpdate(cid);
}

void overlay_show(overlay *o, int cid, uint32_t target, CGRect frame, const overlay_style *style) {
  update(o, cid, target, frame, *style, true);
}

void overlay_move(overlay *o, int cid, uint32_t target, CGRect frame) {
  if (o->shown) update(o, cid, target, frame, o->style, false);
}

static void transact(overlay *o, int cid, int order, uint32_t target, const float *alpha) {
  CFTypeRef transaction = sl.TransactionCreate(cid);
  if (!transaction) return;
  for (int i = 0; i < o->count; i++) {
    if (alpha) sl.TransactionSetWindowAlpha(transaction, o->wid[i], *alpha);
    else sl.TransactionOrderWindow(transaction, o->wid[i], order, target);
  }
  sl.TransactionCommit(transaction, 0);
  CFRelease(transaction);
}

void overlay_restack(overlay *o, int cid, uint32_t target) {
  if (o->shown) transact(o, cid, ORDER_BELOW, target, NULL);
}

void overlay_set_alpha(overlay *o, int cid, float alpha) {
  if (o->shown && alpha != o->style.alpha) transact(o, cid, 0, 0, &alpha);
  o->style.alpha = alpha;
}

void overlay_hide(overlay *o, int cid) {
  if (o->shown) transact(o, cid, ORDER_OUT, 0, NULL);
  o->shown = false;
}

void overlay_destroy(overlay *o, int cid) {
  destroy_strips(o, cid);
}
