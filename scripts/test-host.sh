#!/usr/bin/env bash
# Build and run the host (Linux) unit tests. Extra args go to doctest.
# NOTE: xmake must be run with `-P .` inside tests/ or it picks the root project.
set -euo pipefail
cd "$(dirname "$0")/../tests"
xmake f -P . -p linux -a x86_64 -m debug --cc=clang-22 --cxx=clang++-22 --ld=clang++-22 -y
xmake -P . -y
xmake run -P . dh_core_tests "$@"
