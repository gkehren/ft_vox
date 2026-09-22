## 2024-05-24 - Hot Loop Heap Allocations
**Learning:** Instantiating node-based containers like `std::unordered_set` inside per-frame hot loops (like `uploadPendingMeshes`) causes unnecessary dynamic heap allocations and cache misses.
**Action:** Replace small, short-lived sets in hot loops with `thread_local std::vector` combined with `std::find()`. This achieves better cache locality and eliminates per-frame heap allocations.
