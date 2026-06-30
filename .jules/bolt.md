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
## 2024-06-22 - [TextRenderer String Passing Optimization]
**Learning:** Passing strings by value into hot rendering loop functions incurs significant overhead due to memory allocation and copying. Changing this to `std::string_view` (since we are on C++20) provides an immediate 8% measurable performance boost by completely eliminating these string copies while maintaining modern code cleanliness. Range-based for loops over `std::string_view` are also cleaner than `const_iterator`.
**Action:** When inspecting functions that process read-only text, especially in hot rendering/update paths, always refactor pass-by-value `std::string` or `const std::string&` to `std::string_view` where appropriate.
## 2026-06-22 - [Networking Memory Optimization]
**Learning:** Network message serialization constructs temporary std::vector arrays with predictable sizes based on fixed header values and dynamic payload sizes.
**Action:** When creating a std::vector and sequentially pushing elements, pre-calculate the total expected capacity and call `.reserve()` to prevent intermediary reallocation costs. Never commit temporary benchmark executables to the repo.
## 2025-02-21 - Event Bus Optimization
**Learning:** In highly accessed event systems like `EventBus::publish`, mapping an enum to a vector of handlers via `std::unordered_map` introduces hashing overhead, double lookups (`find` then `[]`), and pointer chasing that degrades performance.
**Action:** Replace `std::unordered_map` with a flat `std::array` indexed by a `Count` element on the enum. This provides O(1) contiguous memory access, completely eliminating hashing and cache misses.
## 2024-06-23 - ChunkManager Container Optimization
**Learning:** In a highly dynamic system like `ChunkManager`, tracking active chunks with `std::unordered_set` incurs significant hashing overhead, individual heap allocations per node, and poor cache locality during iteration (which happens multiple times per frame).
**Action:** Replace `std::unordered_set<Chunk*>` with `std::vector<Chunk*>` for `activeChunks` when the primary operations are iteration and additions, and use (1)$ swap-and-pop for removals. This improves cache locality and iteration speed significantly.
## 2026-06-30 - Shader Uniform Lookup Optimization
**Learning:** Mapping a string literal or `std::string_view` to a cache (like `std::unordered_map<std::string, GLint>`) forces an implicit `std::string` allocation per lookup, which is disastrous in hot loops like rendering.
**Action:** Use a transparent hash functor (`is_transparent = void`) and `std::equal_to<>` with `std::unordered_map` (C++20 heterogeneous lookup) to allow lookups by `std::string_view` directly, completely avoiding allocations on cache hits. Only construct a `std::string` upon cache miss to pass to the C API.
