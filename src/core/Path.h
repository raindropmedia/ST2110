#pragma once

#include <cstddef>

namespace st2110 {

// Empfangspfad nach ST 2022-7: primär (P) und redundant (R).
enum class Path : int { Primary = 0, Redundant = 1 };

inline constexpr size_t kPathCount = 2;

inline const char* pathKey(Path path) {
    return path == Path::Primary ? "P" : "R";
}

inline size_t pathIndex(Path path) {
    return static_cast<size_t>(path);
}

} // namespace st2110
