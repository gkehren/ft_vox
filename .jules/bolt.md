## 2024-05-24 - Sorting Optimization
**Learning:** `std::sort` recalculates its lambda arguments dynamically. When the arguments involve complex calculations like vector math (distance computation), computing them inside the sorting loop results in redundant $O(N \log N)$ calculations instead of $O(N)$.
**Action:** Always pre-calculate expensive metrics before sorting, often using a "Schwartzian transform" pattern (e.g., storing the metric in a `std::pair` or a custom struct alongside the object pointer) to dramatically reduce processing time.
## 2026-06-12 - Thread-Local Buffer Expansion
**Learning:** Extending the existing `thread_local GenBuffers` struct to replace remaining dynamically allocated `std::vector` variables inside terrain generation avoids constant heap allocations without architectural overhauls. Using `std::vector::assign` within these static contexts prevents buffer overflows while continuing to safely reuse existing heap capacity.
**Action:** Always scan hot procedural generation loops for ephemeral `std::vector` instances and promote them to thread-local or persistent buffer objects.
