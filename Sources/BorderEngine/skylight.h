// Private SkyLight (WindowServer client) functions. They are resolved with dlsym at
// startup instead of being linked, so a symbol Apple removes disables the engine rather
// than stopping the app from launching.
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>

typedef struct {
  uint32_t high, low;  // ProcessSerialNumber layout
} sl_psn;

typedef void (*sl_notify_fn)(uint32_t event, void *data, size_t length, void *context);

// X(return type, field, symbol, parameters)
#define SL_REQUIRED(X)                                                                          \
  X(int, MainConnectionID, "SLSMainConnectionID", (void))                                       \
  X(CGError, NewConnection, "SLSNewConnection", (int zero, int *cid))                           \
  X(CGError, RegisterNotifyProc, "SLSRegisterNotifyProc",                                       \
    (sl_notify_fn handler, uint32_t event, void *context))                                      \
  X(CGError, RequestNotificationsForWindows, "SLSRequestNotificationsForWindows",               \
    (int cid, const uint32_t *wids, int count))                                                 \
  X(CGError, GetWindowOwner, "SLSGetWindowOwner", (int cid, uint32_t wid, int *owner_cid))      \
  X(CGError, ConnectionGetPID, "SLSConnectionGetPID", (int cid, pid_t *pid))                    \
  X(OSStatus, GetFrontProcess, "_SLPSGetFrontProcess", (sl_psn * psn))                          \
  X(CGError, GetConnectionIDForPSN, "SLSGetConnectionIDForPSN",                                 \
    (int cid, sl_psn *psn, int *psn_cid))                                                       \
  X(CGError, GetWindowBounds, "SLSGetWindowBounds", (int cid, uint32_t wid, CGRect *bounds))    \
  X(CGError, WindowIsOrderedIn, "SLSWindowIsOrderedIn", (int cid, uint32_t wid, bool *shown))   \
  X(CFArrayRef, CopySpacesForWindows, "SLSCopySpacesForWindows",                                \
    (int cid, int mask, CFArrayRef wids))                                                       \
  X(CGError, MoveWindowsToManagedSpace, "SLSMoveWindowsToManagedSpace",                         \
    (int cid, CFArrayRef wids, uint64_t sid))                                                   \
  X(CFTypeRef, WindowQueryWindows, "SLSWindowQueryWindows",                                     \
    (int cid, CFArrayRef wids, uint32_t options))                                               \
  X(CFTypeRef, WindowQueryResultCopyWindows, "SLSWindowQueryResultCopyWindows",                 \
    (CFTypeRef query))                                                                          \
  X(bool, WindowIteratorAdvance, "SLSWindowIteratorAdvance", (CFTypeRef iterator))              \
  X(uint32_t, WindowIteratorGetWindowID, "SLSWindowIteratorGetWindowID", (CFTypeRef iterator))  \
  X(uint32_t, WindowIteratorGetParentID, "SLSWindowIteratorGetParentID", (CFTypeRef iterator))  \
  X(uint64_t, WindowIteratorGetTags, "SLSWindowIteratorGetTags", (CFTypeRef iterator))          \
  X(int, WindowIteratorGetLevel, "SLSWindowIteratorGetLevel", (CFTypeRef iterator))             \
  X(CGError, NewRegionWithRect, "CGSNewRegionWithRect", (const CGRect *rect, CFTypeRef *region)) \
  X(CGError, NewWindow, "SLSNewWindow",                                                         \
    (int cid, int type, float x, float y, CFTypeRef region, uint32_t *wid))                     \
  X(CGError, ReleaseWindow, "SLSReleaseWindow", (int cid, uint32_t wid))                        \
  /* Sets the window's shape and moves its origin to (x, y). */                                 \
  X(CGError, SetWindowShape, "SLSSetWindowShape",                                               \
    (int cid, uint32_t wid, float x, float y, CFTypeRef region))                                \
  X(CGError, SetWindowLevel, "SLSSetWindowLevel", (int cid, uint32_t wid, int level))           \
  X(CGError, SetWindowTags, "SLSSetWindowTags",                                                 \
    (int cid, uint32_t wid, const uint64_t *tags, int bit_count))                               \
  X(CGError, SetWindowResolution, "SLSSetWindowResolution", (int cid, uint32_t wid, double scale)) \
  X(CGError, SetWindowOpacity, "SLSSetWindowOpacity", (int cid, uint32_t wid, bool opaque))     \
  X(CGContextRef, WindowContextCreate, "SLWindowContextCreate",                                 \
    (int cid, uint32_t wid, CFDictionaryRef options))                                           \
  X(CGError, FlushWindowContentRegion, "SLSFlushWindowContentRegion",                           \
    (int cid, uint32_t wid, void *dirty_region))                                                \
  X(CGError, DisableUpdate, "SLSDisableUpdate", (int cid))                                      \
  X(CGError, ReenableUpdate, "SLSReenableUpdate", (int cid))                                    \
  X(CFTypeRef, TransactionCreate, "SLSTransactionCreate", (int cid))                            \
  X(CGError, TransactionMoveWindowWithGroup, "SLSTransactionMoveWindowWithGroup",               \
    (CFTypeRef transaction, uint32_t wid, CGPoint origin))                                      \
  X(CGError, TransactionOrderWindow, "SLSTransactionOrderWindow",                               \
    (CFTypeRef transaction, uint32_t wid, int order, uint32_t relative_wid))                    \
  X(CGError, TransactionSetWindowAlpha, "SLSTransactionSetWindowAlpha",                         \
    (CFTypeRef transaction, uint32_t wid, float alpha))                                         \
  X(CGError, TransactionCommit, "SLSTransactionCommit", (CFTypeRef transaction, int synchronous))

#define SL_OPTIONAL(X)                                                                          \
  /* macOS 26+: one radius per corner, returned retained. */                                    \
  X(CFArrayRef, WindowIteratorGetCornerRadii, "SLSWindowIteratorGetCornerRadii",                \
    (CFTypeRef iterator))                                                                       \
  X(CGError, WindowSetShadowProperties, "SLSWindowSetShadowProperties",                         \
    (uint32_t wid, CFDictionaryRef properties))                                                 \
  X(CGError, GetWindowAlpha, "SLSGetWindowAlpha", (int cid, uint32_t wid, float *alpha))

#define SL_FIELD(ret, field, symbol, params) ret(*field) params;
extern struct skylight {
  SL_REQUIRED(SL_FIELD)
  SL_OPTIONAL(SL_FIELD)
} sl;
#undef SL_FIELD

// Resolves every symbol. Returns NULL on success, or the first missing required symbol.
const char *sl_load(void);
