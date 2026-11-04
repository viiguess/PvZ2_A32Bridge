#!/bin/sh
# Initializes only the Boost libraries Dynarmic needs (the full superproject is ~170 submodules).
set -e
cd "$(dirname "$0")/.."
git submodule update --init --depth 1 third_party/boost
cd third_party/boost
git fetch --depth 1 origin tag boost-1.85.0 2>/dev/null || true
git checkout -q boost-1.85.0
git submodule update --init --depth 1 -- \
  libs/assert libs/concept_check libs/config libs/container_hash libs/core libs/detail libs/exception libs/functional \
  libs/icl libs/integer libs/iterator libs/move libs/mpl libs/preprocessor libs/range libs/static_assert \
  libs/throw_exception libs/type_index libs/type_traits libs/utility libs/variant
