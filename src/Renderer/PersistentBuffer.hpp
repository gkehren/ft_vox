#pragma once
#include <glad/glad.h>
#include <cstddef>
#include <stdexcept>

class PersistentBuffer {
public:
    PersistentBuffer(GLenum target, size_t size);
    ~PersistentBuffer();

    PersistentBuffer(const PersistentBuffer&) = delete;
    PersistentBuffer& operator=(const PersistentBuffer&) = delete;

    PersistentBuffer(PersistentBuffer&& other) noexcept;
    PersistentBuffer& operator=(PersistentBuffer&& other) noexcept;

    void resize(size_t newSize);
    void* getMappedPtr() const { return mappedPtr; }
    GLuint getID() const { return id; }
    size_t getSize() const { return size; }
    GLenum getTarget() const { return target; }

private:
    void cleanup();

    GLuint id{0};
    GLenum target;
    size_t size{0};
    void* mappedPtr{nullptr};
};
