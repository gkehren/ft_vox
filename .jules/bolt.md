## 2024-05-24 - Sorting Optimization
**Learning:** `std::sort` recalculates its lambda arguments dynamically. When the arguments involve complex calculations like vector math (distance computation), computing them inside the sorting loop results in redundant $O(N \log N)$ calculations instead of $O(N)$.
**Action:** Always pre-calculate expensive metrics before sorting, often using a "Schwartzian transform" pattern (e.g., storing the metric in a `std::pair` or a custom struct alongside the object pointer) to dramatically reduce processing time.
## 2024-05-24 - Priority Queue Construction and Vector Deletion
**Learning:** Initializing a `std::priority_queue` element by element causes $O(N \log N)$ complexity due to individual push operations. Vector `.erase(it)` leads to $O(N^2)$ complexity due to shifting subsequent elements.
**Action:** Use vector iterators `(vec.begin(), vec.end())` to instantiate `std::priority_queue` resulting in an $O(N)$ heapification process. Use the swap and pop idiom `(vec[i] = std::move(vec.back()); vec.pop_back();)` to erase elements from a vector in $O(1)$ time when order does not matter.
