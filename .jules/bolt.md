## 2024-05-24 - Sorting Optimization
**Learning:** `std::sort` recalculates its lambda arguments dynamically. When the arguments involve complex calculations like vector math (distance computation), computing them inside the sorting loop results in redundant $O(N \log N)$ calculations instead of $O(N)$.
**Action:** Always pre-calculate expensive metrics before sorting, often using a "Schwartzian transform" pattern (e.g., storing the metric in a `std::pair` or a custom struct alongside the object pointer) to dramatically reduce processing time.
## 2024-05-25 - Task Scheduling Optimization
**Learning:** For budgeted task queues (where only a few items are selected from a large pool), `std::priority_queue` performs $O(N \log N)$ insertions. When the queue size `N` scales up rapidly, this becomes a severe bottleneck.
**Action:** Replace `std::priority_queue` with a `std::vector` and use `std::partial_sort` to only sort the top `K` elements required by the budget. This reduces overhead from $O(N \log N)$ to roughly $O(N + K \log K)$, which is significantly faster when `K` (budget) is small compared to `N`.
