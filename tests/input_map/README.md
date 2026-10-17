# Input mapping fixture

See [the mapping section of src/input/README.md](../../src/input/README.md#bindings-remapping-and-deadzones).
Build this directory directly with CMake and run CTest; no root runtime/SDL
configuration is required. Explicit C checks execute in Release even under
NDEBUG; a C++ consumer verifies extern-C linkage and the actual mapping target.

Coverage: key names/aliases and their virtual-key codes (written out
independently of the module), binding strings saved by the first version of
this module, binding parse/format/clear/duplicates/limits, keyboard
resolution and both diagonal rules, whole-vector merging, mapping/disabled or
swapped sticks, deadzones, triggers, immutable names, long binding text,
malformed counts/codes and range/roundtrip properties. Fixtures have no
device, game data, save files or guest code dependencies.
