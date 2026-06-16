## 2024-05-24 - Sorting Optimization
**Learning:** `std::sort` recalculates its lambda arguments dynamically. When the arguments involve complex calculations like vector math (distance computation), computing them inside the sorting loop results in redundant $O(N \log N)$ calculations instead of $O(N)$.
**Action:** Always pre-calculate expensive metrics before sorting, often using a "Schwartzian transform" pattern (e.g., storing the metric in a `std::pair` or a custom struct alongside the object pointer) to dramatically reduce processing time.
## 2025-02-20 - Per-frame allocation bottleneck in Render Loop
**Learning:** In C++ rendering loops like `ChunkManager::drawVisibleChunks`, allocating local `std::vector` instances each frame causes unnecessary dynamic memory allocation overhead.
**Action:** Move local vectors used in hot loops to class members, call `.clear()` to maintain capacity and use them to avoid allocations.
