## 2024-05-24 - Sorting Optimization
**Learning:** `std::sort` recalculates its lambda arguments dynamically. When the arguments involve complex calculations like vector math (distance computation), computing them inside the sorting loop results in redundant $O(N \log N)$ calculations instead of $O(N)$.
**Action:** Always pre-calculate expensive metrics before sorting, often using a "Schwartzian transform" pattern (e.g., storing the metric in a `std::pair` or a custom struct alongside the object pointer) to dramatically reduce processing time.

## 2026-06-08 - Heap construction performance
**Learning:** `std::priority_queue::push()` inside a loop performs a heap insertion for every element, resulting in $O(N \log N)$ complexity.
**Action:** When populating a queue, push elements into a `std::vector` first, then construct the `std::priority_queue` directly from the vector using `std::move`. This utilizes `std::make_heap` internally which runs in $O(N)$ time.
