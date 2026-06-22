## 2024-05-24 - Sorting Optimization
**Learning:** `std::sort` recalculates its lambda arguments dynamically. When the arguments involve complex calculations like vector math (distance computation), computing them inside the sorting loop results in redundant $O(N \log N)$ calculations instead of $O(N)$.
**Action:** Always pre-calculate expensive metrics before sorting, often using a "Schwartzian transform" pattern (e.g., storing the metric in a `std::pair` or a custom struct alongside the object pointer) to dramatically reduce processing time.
## 2024-05-24 - Efficient std::vector item removal and std::priority_queue initialization
**Learning:** `std::vector::erase` operates in $O(N)$ leading to $O(N^2)$ execution times if multiple tasks finish per frame (budget allows up to 5000/sec). Furthermore, `std::priority_queue::push` inserts items in $O(\log N)$, but $N$ items pushes are $O(N \log N)$.
**Action:** When item order does not matter in active queues, always prefer to use $O(1)$ swap and pop mechanics (`*it = std::move(vec.back()); vec.pop_back()`). Pre-fill vectors with active items, and initialize a `std::priority_queue` with the underlying vector at once to build the heap in $O(N)$ execution time.
## 2025-02-20 - Per-frame allocation bottleneck in Render Loop
**Learning:** In C++ rendering loops like `ChunkManager::drawVisibleChunks`, allocating local `std::vector` instances each frame causes unnecessary dynamic memory allocation overhead.
**Action:** Move local vectors used in hot loops to class members, call `.clear()` to maintain capacity and use them to avoid allocations.
## 2026-06-22 - [Stream Buffer Flush Optimization]
**Learning:** [std::endl forces a buffer flush on standard streams, which can cause significant I/O performance bottlenecks if used excessively or in hot paths.]
**Action:** [Prefer using `\n` over `std::endl` for C++ log outputs to let the stream manage its buffer efficiently.]
