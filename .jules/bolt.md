## 2024-05-29 - Avoid per-chunk redundant uniform updates
**Learning:** During the shadow pass, setting a uniform matrix to the identity matrix `glm::mat4(1.0f)` for *every single chunk* inside the rendering loop caused thousands of redundant `glUniformMatrix4fv` and `glGetUniformLocation` calls. These calls have a significant CPU overhead due to string hashing and driver communication.
**Action:** Always verify if a uniform is genuinely changing per-object in a rendering loop. If it is constant (like an identity model matrix for chunks in world space), set it once before the loop to drastically reduce API overhead.
## 2024-05-18 - Avoid std::pow for simple integer powers and glm::distance for simple priority sorting
**Learning:** Math functions like `std::pow` with a constant exponent `2.0f` and `glm::distance` are unnecessarily expensive in tight loops, particularly terrain generation and chunk meshing tasks.
**Action:** Replace `std::pow(..., 2.0f)` with a simple `val * val` multiplication, and use squared distances avoiding `std::sqrt` for thresholds and simple distance-based priorities.
