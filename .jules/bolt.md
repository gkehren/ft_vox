## 2024-05-24 - Sorting Optimization
**Learning:** `std::sort` recalculates its lambda arguments dynamically. When the arguments involve complex calculations like vector math (distance computation), computing them inside the sorting loop results in redundant $O(N \log N)$ calculations instead of $O(N)$.
**Action:** Always pre-calculate expensive metrics before sorting, often using a "Schwartzian transform" pattern (e.g., storing the metric in a `std::pair` or a custom struct alongside the object pointer) to dramatically reduce processing time.
## 2024-05-24 - O(1) Erase in Vectors
**Learning:** Removing an element from a `std::vector` inside a loop using `.erase()` is $O(N^2)$ and causes major slowdowns, especially in multi-threaded task management loops where a lock is held. Order does not matter for independent finished tasks.
**Action:** Replace `std::vector::erase` with a swap-and-pop technique (`std::swap(*it, vec.back()); vec.pop_back();`), but handle the last element gracefully to prevent invalidating iterators by breaking early.
