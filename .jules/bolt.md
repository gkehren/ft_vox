## 2024-05-24 - Sorting Optimization
**Learning:** `std::sort` recalculates its lambda arguments dynamically. When the arguments involve complex calculations like vector math (distance computation), computing them inside the sorting loop results in redundant $O(N \log N)$ calculations instead of $O(N)$.
**Action:** Always pre-calculate expensive metrics before sorting, often using a "Schwartzian transform" pattern (e.g., storing the metric in a `std::pair` or a custom struct alongside the object pointer) to dramatically reduce processing time.
## 2025-02-23 - std::priority_queue initialization
**Learning:** Inserting elements one-by-one into a `std::priority_queue` takes $O(N \log N)$ time. By instead using a `std::vector` with `reserve()` to gather elements and using the `std::priority_queue` constructor that takes an rvalue container (`std::move`), the underlying heap is built in $O(N)$ time using `std::make_heap`.
**Action:** Always prefer initializing `std::priority_queue` with a pre-populated `std::vector` ($O(N)$) rather than using individual `push()` calls ($O(N \log N)$).
