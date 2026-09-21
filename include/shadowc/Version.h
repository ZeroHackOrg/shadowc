#ifndef SHADOWC_INCLUDE_SHADOWC_VERSION_H
#define SHADOWC_INCLUDE_SHADOWC_VERSION_H

// Version of the shadowc framework. Mirrored in pyproject.toml and the
// Python package (src/shadowc/__init__.py). Bump all four together.
#define SHADOWC_VERSION_MAJOR 0
#define SHADOWC_VERSION_MINOR 5
#define SHADOWC_VERSION_PATCH 0

#define SHADOWC_STRINGIFY_IMPL(x) #x
#define SHADOWC_STRINGIFY(x) SHADOWC_STRINGIFY_IMPL(x)

#define SHADOWC_VERSION                                         \
  SHADOWC_STRINGIFY(SHADOWC_VERSION_MAJOR) "." SHADOWC_STRINGIFY( \
      SHADOWC_VERSION_MINOR) "." SHADOWC_STRINGIFY(SHADOWC_VERSION_PATCH)

// Human readable tag shown by `shadowc --version` and the plugin loader.
#define SHADOWC_FRAMEWORK_NAME "shadowc"

#endif // SHADOWC_INCLUDE_SHADOWC_VERSION_H