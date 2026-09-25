// Compiled separately into a shared library and loaded into a session with %lib,
// to show that library code runs at its own build's optimisation level.
#include <cstdint>

extern "C" bool within_band(std::int64_t px, std::int64_t ref, std::int64_t band) {
    const std::int64_t d = px - ref;
    return d <= band && d >= -band;
}
