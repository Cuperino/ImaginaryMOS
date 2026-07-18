<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Documentation

Developer and user docs (plan §15). See
[coding-standards.md](coding-standards.md) and
[threat-model.md](threat-model.md).

## API reference + guides (Doxygen)

`docs/api/` holds narrative guide pages — a getting-started/setup walkthrough,
an architecture overview, and a dedicated page for each MOS profile (0–7) with
worked examples for both roles — rendered together with the extracted C++ API
reference by Doxygen. Build them with:

```sh
cmake --preset linux -DIMOS_BUILD_DOCS=ON
cmake --build --preset linux --target docs
# open build/linux/docs/html/index.html
```

The `docs` target self-gates on `IMOS_BUILD_DOCS` (off by default) and the
presence of `doxygen`; `graphviz`/`dot` is used for diagrams when available.
The configuration lives in [`Doxyfile.in`](Doxyfile.in) (CMake-substituted) and
[`CMakeLists.txt`](CMakeLists.txt).

The remaining plan §15 deliverables — the dialect handbook and the
porting/embedding guide — are still drafted in **M7**.
