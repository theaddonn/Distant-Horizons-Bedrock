# Vendored header-only dependencies of dh_core

Copied verbatim so the Windows clang-cl build and the Linux host build see identical headers.

| Name | Version | License | Upstream | Location |
|---|---|---|---|---|
| GLM | 1.0.1 | MIT (or Happy Bunny) | https://github.com/g-truc/glm | `glm/` |
| parallel-hashmap (phmap) | 1.3.12 | Apache-2.0 | https://github.com/greg7mdp/parallel-hashmap | `parallel_hashmap/` |
| Microsoft GSL | 4.2.0 | MIT | https://github.com/microsoft/GSL | `gsl/` |
| expected-lite | 0.8.0 | BSL-1.0 | https://github.com/martinmoene/expected-lite | `nonstd/expected.hpp` |
| moodycamel concurrentqueue | 1.0.4 | BSD-2-Clause / Boost (dual) | https://github.com/cameron314/concurrentqueue | `moodycamel/`, `concurrentqueue.h`, `blockingconcurrentqueue.h` (wrappers) |
| nlohmann/json | 3.12.0 | MIT | https://github.com/nlohmann/json | `nlohmann/json.hpp` (single header) |

Notes:
- `glm/CMakeLists.txt` and `glm/glm.cppm` were removed.
- `concurrentqueue.h` / `blockingconcurrentqueue.h` at this directory's root are thin wrappers that include `moodycamel/*.h`, so both `#include <concurrentqueue.h>` and `#include <moodycamel/concurrentqueue.h>` work.
- Sources: xmake package cache (glm, phmap, gsl, expected-lite, concurrentqueue) and the upstream release `json.hpp` (nlohmann).
