#include "border_engine.h"
#include "overlay.h"
#include "skylight.h"
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// WindowServer notifications. Per-window events (8xx) arrive only for windows passed
// to SLSRequestNotificationsForWindows.
enum {
  EVENT_WINDOW_CLOSE = 804,
  EVENT_WINDOW_MOVE = 806,
  EVENT_WINDOW_RESIZE = 807,
  EVENT_WINDOW_REORDER = 808,
  EVENT_WINDOW_LEVEL = 811,
  EVENT_WINDOW_UNHIDE = 815,  // also: restored from the Dock
  EVENT_WINDOW_HIDE = 816,    // also: minimized (sent when the animation ends)
  EVENT_SPACE_ADD = 1325,     // window joined a Space (first shown); payload: u64 sid, u32 wid
  EVENT_SPACE_REMOVE = 1326,  // window left a Space (closed or moved); same payload
  EVENT_DOCK_ANIMATION_BEGIN = 1327,  // minimize/restore (genie) starts; payload doesn't name the window
  EVENT_DOCK_ANIMATION_END = 1328,
  EVENT_SPACE_CHANGE = 1401,
  EVENT_FRONT_APP_CHANGE = 1508,
};

#define TAG_DOCUMENT (1ULL << 0)         // set on ordinary app windows
#define TAG_DOCK_ANIMATING (1ULL << 34)  // set while the window is being minimized/restored
#define MIN_WINDOW_SIZE 40.0
#define FALLBACK_CORNER_RADIUS 10.0f  // macOS 15 and earlier
#define INACTIVE_ALPHA 0.5f
#define DEFAULT_WIDTH 4.0f
#define MIN_WIDTH 1.0f
#define MAX_WIDTH 40.0f
#define ALL_SPACES_MASK 0x7

#define LOG(...)                                       \
  do {                                                 \
    if (E.debug) {                                     \
      fprintf(stderr, "[engine] " __VA_ARGS__);        \
      fputc('\n', stderr);                             \
    }                                                  \
  } while (0)

typedef struct {
  uint32_t wid;
  pid_t pid;
  bool styled;     // color_fn wants a border for this app
  bool dirty;      // refresh on the next flush
  bool animating;  // hidden while the Dock animates it; refreshed when the animation ends
  be_color color;
  overlay overlay;
} tracked_window;

typedef struct {
  CGRect frame;
  uint64_t tags;
  uint32_t parent;
  int level;
  float radius;
} window_info;

static struct {
  int cid;         // main connection: queries and notifications
  int border_cid;  // separate connection owning the overlay windows
  pid_t pid;
  bool started, enabled, debug;
  const char *missing;
  be_color_fn color_fn;
  void *color_context;
  float width;
  tracked_window *windows;
  int count, capacity;
  uint32_t focused;
  bool flush_pending, focus_pending, reconcile_pending, watch_list_dirty;
} E;

// MARK: - Window queries

static CFArrayRef wid_array(uint32_t wid) {
  CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &wid);
  CFArrayRef list = CFArrayCreate(NULL, (const void **)&number, 1, &kCFTypeArrayCallBacks);
  CFRelease(number);
  return list;
}

static float corner_radius(CFTypeRef iterator) {
  if (!sl.WindowIteratorGetCornerRadii) return FALLBACK_CORNER_RADIUS;
  float radius = 0;
  CFArrayRef radii = sl.WindowIteratorGetCornerRadii(iterator);
  if (radii) {
    if (CFArrayGetCount(radii) > 0)
      CFNumberGetValue(CFArrayGetValueAtIndex(radii, 0), kCFNumberFloatType, &radius);
    CFRelease(radii);
  }
  return radius;
}

// Returns false if the window no longer exists.
static bool query_window(uint32_t wid, window_info *info) {
  CFArrayRef list = wid_array(wid);
  CFTypeRef query = sl.WindowQueryWindows(E.cid, list, 0);
  CFTypeRef iterator = query ? sl.WindowQueryResultCopyWindows(query) : NULL;
  bool found = iterator && sl.WindowIteratorAdvance(iterator);
  if (found) {
    info->tags = sl.WindowIteratorGetTags(iterator);
    info->parent = sl.WindowIteratorGetParentID(iterator);
    info->level = sl.WindowIteratorGetLevel(iterator);
    info->radius = corner_radius(iterator);
  }
  if (iterator) CFRelease(iterator);
  if (query) CFRelease(query);
  CFRelease(list);
  return found && sl.GetWindowBounds(E.cid, wid, &info->frame) == kCGErrorSuccess;
}

static uint64_t window_space(uint32_t wid) {
  CFArrayRef list = wid_array(wid);
  CFArrayRef spaces = sl.CopySpacesForWindows(E.cid, ALL_SPACES_MASK, list);
  CFRelease(list);
  uint64_t sid = 0;
  if (spaces) {
    if (CFArrayGetCount(spaces) > 0)
      CFNumberGetValue(CFArrayGetValueAtIndex(spaces, 0), kCFNumberSInt64Type, &sid);
    CFRelease(spaces);
  }
  return sid;
}

static pid_t window_pid(uint32_t wid) {
  int owner = 0;
  pid_t pid = 0;
  if (sl.GetWindowOwner(E.cid, wid, &owner) == kCGErrorSuccess) sl.ConnectionGetPID(owner, &pid);
  return pid;
}

// Ordinary app windows: top level, normal level, document tag, on a Space. This skips
// menu bar strips, popups, placeholders, sheets and panels.
static bool is_candidate(const window_info *info, uint64_t space) {
  return info->parent == 0 && info->level == 0 && (info->tags & TAG_DOCUMENT) && space != 0 &&
         info->frame.size.width >= MIN_WINDOW_SIZE && info->frame.size.height >= MIN_WINDOW_SIZE;
}

// MARK: - Tracking

static tracked_window *find(uint32_t wid) {
  for (int i = 0; i < E.count; i++)
    if (E.windows[i].wid == wid) return &E.windows[i];
  return NULL;
}

static float alpha_for(const tracked_window *w) {
  return w->wid == E.focused ? 1.0f : INACTIVE_ALPHA;
}

static void untrack(tracked_window *w) {
  LOG("untrack %u", w->wid);
  overlay_destroy(&w->overlay, E.border_cid);
  if (E.focused == w->wid) E.focused = 0;
  *w = E.windows[--E.count];
  E.watch_list_dirty = true;
}

// Brings the overlay in line with the window's current state.
static void refresh(tracked_window *w) {
  window_info info;
  if (!query_window(w->wid, &info)) {
    untrack(w);
    return;
  }
  bool ordered_in = false;
  sl.WindowIsOrderedIn(E.cid, w->wid, &ordered_in);
  w->animating = (info.tags & TAG_DOCK_ANIMATING) != 0;
  if (!E.enabled || !w->styled || !ordered_in || w->animating) {
    overlay_hide(&w->overlay, E.border_cid);
    return;
  }
  overlay_style style = {
      .width = E.width,
      .radius = info.radius,
      .color = w->color,
      .alpha = alpha_for(w),
      .space = window_space(w->wid),
      .level = info.level,
  };
  overlay_show(&w->overlay, E.border_cid, w->wid, info.frame, &style);
}

typedef enum { TRACK_ADDED, TRACK_KNOWN, TRACK_IGNORED, TRACK_NOT_READY } track_result;

static track_result track(uint32_t wid) {
  if (find(wid)) return TRACK_KNOWN;
  pid_t pid = window_pid(wid);
  if (pid <= 0 || pid == E.pid) return TRACK_IGNORED;
  window_info info;
  if (!query_window(wid, &info) || !is_candidate(&info, window_space(wid))) return TRACK_NOT_READY;

  if (E.count == E.capacity) {
    int capacity = E.capacity ? E.capacity * 2 : 64;
    tracked_window *grown = realloc(E.windows, sizeof(tracked_window) * capacity);
    if (!grown) return TRACK_IGNORED;
    E.windows = grown;
    E.capacity = capacity;
  }
  tracked_window *w = &E.windows[E.count++];
  memset(w, 0, sizeof *w);
  w->wid = wid;
  w->pid = pid;
  w->styled = E.color_fn(pid, &w->color, E.color_context);
  E.watch_list_dirty = true;
  LOG("track %u pid=%d styled=%d frame=(%.0f,%.0f %.0fx%.0f) radius=%.0f", wid, pid, w->styled,
      info.frame.origin.x, info.frame.origin.y, info.frame.size.width, info.frame.size.height,
      info.radius);
  refresh(w);
  return TRACK_ADDED;
}

static void update_watch_list(void) {
  E.watch_list_dirty = false;
  uint32_t *wids = malloc(sizeof(uint32_t) * (E.count ? E.count : 1));
  if (!wids) return;
  for (int i = 0; i < E.count; i++) wids[i] = E.windows[i].wid;
  sl.RequestNotificationsForWindows(E.cid, wids, E.count);
  free(wids);
}

static void discover(CGWindowListOption option) {
  CFArrayRef wids = CGWindowListCreate(option, kCGNullWindowID);
  if (!wids) return;
  for (CFIndex i = 0; i < CFArrayGetCount(wids); i++)
    track((uint32_t)(uintptr_t)CFArrayGetValueAtIndex(wids, i));
  CFRelease(wids);
}

// MARK: - Focus

// The focused window is the frontmost on-screen window of the frontmost app.
static uint32_t front_window(void) {
  sl_psn psn;
  int front_cid = 0;
  pid_t front_pid = 0;
  if (sl.GetFrontProcess(&psn) != 0) return 0;
  sl.GetConnectionIDForPSN(E.cid, &psn, &front_cid);
  sl.ConnectionGetPID(front_cid, &front_pid);
  if (front_pid == E.pid) return E.focused;  // our menu or colour panel: keep the current focus

  uint32_t result = 0;
  CFArrayRef wids = CGWindowListCreate(
      kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
  if (!wids) return 0;
  for (CFIndex i = 0; i < CFArrayGetCount(wids); i++) {  // front to back
    tracked_window *w = find((uint32_t)(uintptr_t)CFArrayGetValueAtIndex(wids, i));
    if (w && w->pid == front_pid) {
      result = w->wid;
      break;
    }
  }
  CFRelease(wids);
  return result;
}

static void update_focus(void) {
  uint32_t focused = front_window();
  if (focused == E.focused) return;
  LOG("focus %u -> %u", E.focused, focused);
  tracked_window *previous = find(E.focused);
  E.focused = focused;
  if (previous) overlay_set_alpha(&previous->overlay, E.border_cid, INACTIVE_ALPHA);
  tracked_window *current = find(focused);
  if (current) overlay_set_alpha(&current->overlay, E.border_cid, 1.0f);
}

// MARK: - Scheduling (coalesces bursts of events into one pass)

static void flush(void) {
  for (int i = E.count - 1; i >= 0; i--) {  // backwards: refresh may untrack
    if (!E.windows[i].dirty) continue;
    E.windows[i].dirty = false;
    refresh(&E.windows[i]);
  }
  if (E.watch_list_dirty) update_watch_list();
}

static void schedule_flush(void) {
  if (E.flush_pending) return;
  E.flush_pending = true;
  dispatch_async(dispatch_get_main_queue(), ^{
    E.flush_pending = false;
    flush();
  });
}

static void mark_dirty(tracked_window *w) {
  w->dirty = true;
  schedule_flush();
}

static void schedule_focus(int64_t delay_ms) {
  if (E.focus_pending) return;
  E.focus_pending = true;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay_ms * NSEC_PER_MSEC),
                 dispatch_get_main_queue(), ^{
                   E.focus_pending = false;
                   update_focus();
                 });
}

// Re-checks every tracked window and picks up visible windows we missed.
static void schedule_reconcile(int64_t delay_ms) {
  if (E.reconcile_pending) return;
  E.reconcile_pending = true;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay_ms * NSEC_PER_MSEC),
                 dispatch_get_main_queue(), ^{
                   E.reconcile_pending = false;
                   for (int i = 0; i < E.count; i++) E.windows[i].dirty = true;
                   flush();
                   discover(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements);
                   if (E.watch_list_dirty) update_watch_list();
                   update_focus();
                 });
}

// The genie animation moves the window without move events, and the hide event only
// arrives once it has finished. Hide the border as soon as it starts instead. The begin
// event doesn't say which window animates, but that window carries TAG_DOCK_ANIMATING.
static void hide_animating_windows(void) {
  if (!E.count) return;
  CFMutableArrayRef list = CFArrayCreateMutable(NULL, E.count, &kCFTypeArrayCallBacks);
  for (int i = 0; i < E.count; i++) {
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &E.windows[i].wid);
    CFArrayAppendValue(list, number);
    CFRelease(number);
  }
  CFTypeRef query = sl.WindowQueryWindows(E.cid, list, 0);
  CFTypeRef iterator = query ? sl.WindowQueryResultCopyWindows(query) : NULL;
  while (iterator && sl.WindowIteratorAdvance(iterator)) {
    if (!(sl.WindowIteratorGetTags(iterator) & TAG_DOCK_ANIMATING)) continue;
    tracked_window *w = find(sl.WindowIteratorGetWindowID(iterator));
    if (!w) continue;
    LOG("animating %u: hide border", w->wid);
    w->animating = true;
    overlay_hide(&w->overlay, E.border_cid);
  }
  if (iterator) CFRelease(iterator);
  if (query) CFRelease(query);
  CFRelease(list);
}

static void finish_animations(void) {
  for (int i = 0; i < E.count; i++)
    if (E.windows[i].animating) mark_dirty(&E.windows[i]);
}

// A window that just joined a Space may not be fully set up yet; look again shortly.
static void retrack_later(uint32_t wid) {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    tracked_window *w = find(wid);
    if (w) refresh(w);
    else track(wid);
    if (E.watch_list_dirty) update_watch_list();
  });
}

// MARK: - Events

static void handle_event(uint32_t event, void *data, size_t length, void *context) {
  uint32_t wid = 0;
  if (event == EVENT_SPACE_ADD || event == EVENT_SPACE_REMOVE) {
    if (length >= sizeof(uint64_t) + sizeof(uint32_t)) memcpy(&wid, (char *)data + sizeof(uint64_t), sizeof wid);
  } else if (length >= sizeof wid) {
    memcpy(&wid, data, sizeof wid);
  }
  tracked_window *w = wid ? find(wid) : NULL;

  switch (event) {
  case EVENT_SPACE_ADD:
    if (w) mark_dirty(w);
    else if (track(wid) == TRACK_IGNORED) break;  // our own overlay windows land here
    else schedule_flush();                         // sends the updated watch list
    retrack_later(wid);
    schedule_focus(30);
    break;
  case EVENT_SPACE_REMOVE:
    if (w) mark_dirty(w);
    schedule_focus(30);
    break;
  case EVENT_WINDOW_CLOSE:
    if (w) {
      untrack(w);
      schedule_flush();
    }
    schedule_focus(30);
    break;
  case EVENT_WINDOW_MOVE:
    if (w) {
      CGRect frame;
      if (sl.GetWindowBounds(E.cid, wid, &frame) == kCGErrorSuccess)
        overlay_move(&w->overlay, E.border_cid, wid, frame);
    }
    break;
  case EVENT_WINDOW_RESIZE:
  case EVENT_WINDOW_LEVEL:
  case EVENT_WINDOW_UNHIDE:
    if (w) mark_dirty(w);
    break;
  case EVENT_WINDOW_HIDE:
    if (w) {
      overlay_hide(&w->overlay, E.border_cid);
      mark_dirty(w);
    }
    schedule_focus(30);
    break;
  case EVENT_WINDOW_REORDER:
    if (w) overlay_restack(&w->overlay, E.border_cid, wid);
    schedule_focus(10);
    break;
  case EVENT_DOCK_ANIMATION_BEGIN:
    hide_animating_windows();
    // Fallback in case the end event goes missing.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
      finish_animations();
    });
    break;
  case EVENT_DOCK_ANIMATION_END:
    finish_animations();
    break;
  case EVENT_SPACE_CHANGE:
    schedule_reconcile(50);  // fullscreen windows update their Space a little later
    break;
  case EVENT_FRONT_APP_CHANGE:
    schedule_focus(30);
    break;
  }
}

// MARK: - Public API

bool be_start(be_color_fn color_fn, void *context) {
  if (E.started) return true;
  E.debug = getenv("BORDERCOLOR_DEBUG") != NULL;
  E.missing = sl_load();
  if (E.missing) return false;
  if (sl.NewConnection(0, &E.border_cid) != kCGErrorSuccess) {
    E.missing = "SLSNewConnection";
    return false;
  }
  E.cid = sl.MainConnectionID();
  E.pid = getpid();
  E.color_fn = color_fn;
  E.color_context = context;
  E.enabled = true;
  if (!E.width) E.width = DEFAULT_WIDTH;

  static const uint32_t events[] = {
      EVENT_WINDOW_CLOSE,         EVENT_WINDOW_MOVE,        EVENT_WINDOW_RESIZE,
      EVENT_WINDOW_REORDER,       EVENT_WINDOW_LEVEL,       EVENT_WINDOW_UNHIDE,
      EVENT_WINDOW_HIDE,          EVENT_SPACE_ADD,          EVENT_SPACE_REMOVE,
      EVENT_DOCK_ANIMATION_BEGIN, EVENT_DOCK_ANIMATION_END, EVENT_SPACE_CHANGE,
      EVENT_FRONT_APP_CHANGE,
  };
  for (size_t i = 0; i < sizeof events / sizeof *events; i++)
    sl.RegisterNotifyProc(handle_event, events[i], NULL);

  discover(kCGWindowListOptionAll);  // includes windows on other Spaces and minimized ones
  update_watch_list();
  update_focus();
  E.started = true;
  LOG("started: tracking %d windows", E.count);
  return true;
}

void be_set_enabled(bool enabled) {
  if (!E.started || E.enabled == enabled) {
    E.enabled = enabled;
    return;
  }
  E.enabled = enabled;
  for (int i = 0; i < E.count; i++) E.windows[i].dirty = true;
  flush();
}

void be_set_width(double width) {
  float clamped = width < MIN_WIDTH ? MIN_WIDTH : width > MAX_WIDTH ? MAX_WIDTH : (float)width;
  if (clamped == E.width) return;
  E.width = clamped;
  if (!E.started) return;
  for (int i = 0; i < E.count; i++) E.windows[i].dirty = true;
  flush();
}

void be_refresh_colors(void) {
  if (!E.started) return;
  for (int i = 0; i < E.count; i++) {
    tracked_window *w = &E.windows[i];
    w->styled = E.color_fn(w->pid, &w->color, E.color_context);
    w->dirty = true;
  }
  flush();
}

const char *be_missing_symbol(void) {
  return E.missing ? E.missing : "";
}
