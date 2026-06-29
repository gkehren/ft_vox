## 2024-06-18 - Replacing Unordered Set with Vector for Cache Locality
**Learning:** In a C++ rendering loop, using `std::unordered_set` to track active objects (like `activeChunks`) causes significant cache misses and pointer chasing overhead when iterating.
**Action:** Replace `std::unordered_set` with `std::vector` for collections that are frequently iterated but rarely searched. Use O(1) swap-and-pop (`*it = vec.back(); vec.pop_back();`) for removals when order doesn't matter to avoid shifting elements.
