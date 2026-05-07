#pragma once

#include <v8.h>

namespace fxe::js {
  // Installs the `RenderStats` global namespace on the supplied template.
  // Methods exposed: snapshot(), reset(), recordCacheHit(), recordCacheMiss(),
  // recordRebuild(), recordQueueCall(), beginFrame().
  void install_render_stats_global(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);
} // namespace fxe::js
