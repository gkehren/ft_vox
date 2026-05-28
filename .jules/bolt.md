## 2024-05-18 - Avoid std::pow for simple integer powers and glm::distance for simple priority sorting
**Learning:** Math functions like `std::pow` with a constant exponent `2.0f` and `glm::distance` are unnecessarily expensive in tight loops, particularly terrain generation and chunk meshing tasks.
**Action:** Replace `std::pow(..., 2.0f)` with a simple `val * val` multiplication, and use squared distances avoiding `std::sqrt` for thresholds and simple distance-based priorities.
