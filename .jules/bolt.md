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
## 2024-05-19 - [Avoid unordered_set for Object State Tracking]
**Learning:** Maintaining an external `std::unordered_set<Chunk*>` to track which chunks are currently in transit introduces unnecessary $O(1)$ node-based hashing overhead and cache misses, especially when checked repeatedly inside hot game loops (like frustum culling or chunk meshing).
**Action:** When tracking simple binary state for an object (like "is in transit" or "is processing"), embed an `std::atomic<bool>` flag directly into the object class itself rather than tracking it externally in a hash map. This converts a hash map lookup into a simple inline boolean check, significantly improving cache locality.
## 2024-07-06 - Always Verify Full Method Text Before Edits
**Learning:** Output from tools like `cat` might get truncated if a file is long. Assuming method implementation details based on incomplete output leads to bad plans.
**Action:** Use `read_file` or `grep -A 50` on specific methods to ensure the full implementation is viewed before proposing exact edits.
## 2024-05-24 - Precalculate AABB Optimal Test Corners for Frustum Culling
**Learning:** In broad-phase frustum culling against AABBs, evaluating `aabbMin` vs `aabbMax` for each plane inside the active chunks loop introduces branch mispredictions and redundant calculations since the frustum planes are constant per-frame.
**Action:** Always precalculate the optimal testing corner (the plane offset vector) for each of the 6 frustum planes *outside* the chunk iteration loop. Combine this with precalculated normals and plane offsets (`w`), so the inner loop simply adds the precalculated offset to `aabbMin` and evaluates the dot product without branching.
## 2024-05-24 - Network Packet Hot Loop Optimization
**Learning:** Using `std::unordered_set` dynamically inside frequent network packet handlers (like `Client::handleMessage`) causes node-based heap allocations for every packet processed, leading to unnecessary garbage and potential stutters.
**Action:** For bounded and small collections derived from network packets, use `std::vector` with `reserve()`, sort the vector, and use `std::binary_search()`. This leverages contiguous memory and avoids node allocations entirely.

## 2024-05-18 - ChunkManager Load Queue Deduplication Pitfall
**Learning:** In `ChunkManager`, attempting to remove `m_enqueuedLoads` (`std::unordered_set`) and deduplicate the `m_loadQueue` by calling `std::unique` immediately after sorting by *distance* fails. Identical coordinates might have the exact same distance but are not guaranteed to be adjacent in a stable sort if floating-point calculations differ minutely or symmetrically.
**Action:** Do not deduplicate `m_loadQueue` in-place using `std::unique` after a distance sort. Either maintain the `std::unordered_set`, or re-sort by coordinates before using `std::unique`, or rely on coordinate-based binary searches if removing the set.
## 2024-05-24 - Build System Fallbacks & Experimental Targets
**Learning:** When building this project on Linux environments lacking SDL3 packages, configuring with `cmake -B build-vk -DFT_VOX_FETCH_SDL3=ON -DFT_VOX_DEP_MODE=system` works, but experimental shader targets (`ft_vox_shaders`) may fail to compile due to missing GLSL extensions. Running `make -j4` globally will fail.
**Action:** Instead of a global `make`, explicitly build only the required executable targets (e.g., `make -j4 ft_vox test_network test_stream_opt`) to ensure successful compilation and avoid breaking the pipeline on unneeded targets.

## 2024-05-18 - [ChunkManager Hot-Path Thread-Local Queues]
**Learning:** [The `ChunkManager` relies on dynamically creating `std::vector<Item>` queues inside hot path methods (`generatePendingVoxels`, `meshPendingChunks`, `uploadPendingMeshes`) which are called frequently in the main loop. These dynamic heap allocations can degrade performance. Reusing memory is complex due to `std::shared_mutex` usage and potential concurrency.]
**Action:** [Use `thread_local std::vector<Item>` with a `queue.clear()` at the start of the method to safely reuse allocated capacity across frames without introducing data races or locking overhead in concurrent methods.]

## 2024-05-30 - [Network Receive Hot-Path Micro-Optimization]
**Learning:** Using `thread_local std::vector` inside hot event/async callbacks (like `handleReceive` in Boost.ASIO) completely eliminates dynamic heap allocations on every packet. Because ASIO completions run sequentially on the thread, there are no data races or re-entrancy issues, making TLS safely overwrite previous packet data effectively.
**Action:** When working in high-throughput network receive loops and parsing functions, avoid per-packet dynamic heap allocations by replacing locally scoped vectors with `thread_local std::vector`, reusing capacity via `.assign()` or `.clear()`.
