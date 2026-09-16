
## 2024-05-23 - Avoid std::unordered_set for small bounded subsets in hot loops
**Learning:** `std::unordered_set` has significant overhead due to individual hash node allocations. When iterating over small numbers of items bounded by budgets per frame, a `thread_local std::vector` combined with `std::find` outperforms `std::unordered_set` significantly (3x-4x faster for sets <= 20 elements).
**Action:** Use `thread_local std::vector` instead of node-based associative containers for transient deduplication structures within hot engine loops like mesh uploads.
