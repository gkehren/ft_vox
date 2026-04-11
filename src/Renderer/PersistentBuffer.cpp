#include "PersistentBuffer.hpp"
#include <utility>

PersistentBuffer::PersistentBuffer(GLenum target, size_t size) : target(target) {
    if (size > 0) resize(size);
}

PersistentBuffer::~PersistentBuffer() {
    cleanup();
}

PersistentBuffer::PersistentBuffer(PersistentBuffer&& other) noexcept
    : id(other.id), target(other.target), size(other.size), mappedPtr(other.mappedPtr) {
    other.id = 0;
    other.size = 0;
    other.mappedPtr = nullptr;
}

PersistentBuffer& PersistentBuffer::operator=(PersistentBuffer&& other) noexcept {
    if (this != &other) {
        cleanup();
        id = other.id;
        target = other.target;
        size = other.size;
        mappedPtr = other.mappedPtr;

        other.id = 0;
        other.size = 0;
        other.mappedPtr = nullptr;
    }
    return *this;
}

void PersistentBuffer::resize(size_t newSize) {
    if (size >= newSize && id != 0) return;

    cleanup();
    size = newSize;

    glGenBuffers(1, &id);
    glBindBuffer(target, id);
    
    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glBufferStorage(target, size, nullptr, flags);
    mappedPtr = glMapBufferRange(target, 0, size, flags);
    glBindBuffer(target, 0);

    if (!mappedPtr) {
        throw std::runtime_error("Failed to map persistent buffer");
    }
}

void PersistentBuffer::cleanup() {
    if (id != 0) {
        // glDeleteBuffers automatically unmaps the buffer and cleans up bindings
        glDeleteBuffers(1, &id);
        id = 0;
        mappedPtr = nullptr;
    }
}
