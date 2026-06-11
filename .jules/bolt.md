## 2024-05-24 - Sorting Optimization
**Learning:** `std::sort` recalculates its lambda arguments dynamically. When the arguments involve complex calculations like vector math (distance computation), computing them inside the sorting loop results in redundant $O(N \log N)$ calculations instead of $O(N)$.
**Action:** Always pre-calculate expensive metrics before sorting, often using a "Schwartzian transform" pattern (e.g., storing the metric in a `std::pair` or a custom struct alongside the object pointer) to dramatically reduce processing time.

## 2026-06-11 - Frame Allocations in Hot Paths
**Learning:** Standard C++ containers like `std::vector` are dynamically allocated on the heap by default. Creating and throwing away vectors inside the main rendering loop (like in `drawVisibleChunks`) causes constant allocation and deallocation, thrashing the allocator and reducing performance.
**Action:** Promote local vectors used inside hot loops (like rendering) to mutable class members and use `.clear()` each frame to reuse the previously allocated capacity.
