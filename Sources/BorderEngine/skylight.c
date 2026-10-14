#include "skylight.h"
#include <dlfcn.h>

struct skylight sl;

const char *sl_load(void) {
  void *lib = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
                     RTLD_LAZY | RTLD_LOCAL);
  if (!lib) return "SkyLight.framework";

#define SL_RESOLVE_REQUIRED(ret, field, symbol, params) \
  if (!(sl.field = (ret(*) params)dlsym(lib, symbol))) return symbol;
#define SL_RESOLVE_OPTIONAL(ret, field, symbol, params) \
  sl.field = (ret(*) params)dlsym(lib, symbol);

  SL_REQUIRED(SL_RESOLVE_REQUIRED)
  SL_OPTIONAL(SL_RESOLVE_OPTIONAL)
  return NULL;
}
