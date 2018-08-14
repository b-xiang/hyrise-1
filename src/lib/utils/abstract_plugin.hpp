#pragma once

#include <iostream>

#include "storage/storage_manager.hpp"
#include "types.hpp"
#include "utils/singleton.hpp"

namespace opossum {

// This is necessary to make the plugin instantiable, it leads to plain C linkage to avoid
// ungly mangled names.

#define EXPORT(PluginName)                                            \
  extern "C" AbstractPlugin* factory() {                              \
    auto plugin = static_cast<AbstractPlugin*>(&(PluginName::get())); \
    return plugin;                                                    \
  }

// AbstractPlugin is the abstract super class for all plugins. An example implementation can be found
// under test/utils/test_plugin.cpp. Usually plugins are implemented as singletons because there
// shouldn't be multiple instances of them as they would compete against each other.

class AbstractPlugin {
 public:
  virtual const std::string description() const = 0;

  virtual void start() const = 0;

  virtual void stop() const = 0;
};

}  // namespace opossum
