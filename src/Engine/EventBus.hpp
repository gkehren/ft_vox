// src/Engine/EventBus.hpp
#pragma once
#include <functional>
#include <vector>
#include <memory>
#include <unordered_map>
#include <array>
#include <SDL3/SDL.h>

enum class EventType {
    WindowResize,
    KeyPress,
    KeyRelease,
    MouseMotion,
    MouseButtonPress,
    MouseButtonRelease,
    MouseWheel,
    Quit,
    Count
};

struct Event {
    EventType type;
    virtual ~Event() = default;
};

struct WindowResizeEvent : public Event {
    int width, height;
    WindowResizeEvent(int w, int h) : width(w), height(h) { type = EventType::WindowResize; }
};

struct KeyEvent : public Event {
    SDL_Keycode key;
    KeyEvent(EventType t, SDL_Keycode k) : key(k) { type = t; }
};

struct MouseEvent : public Event {
    int x, y, dx, dy;
    uint8_t button;
    MouseEvent(EventType t, int x, int y, int dx=0, int dy=0, uint8_t b=0) 
        : x(x), y(y), dx(dx), dy(dy), button(b) { type = t; }
};

struct MouseWheelEvent : public Event {
    float x, y;
    MouseWheelEvent(float x, float y) : x(x), y(y) { type = EventType::MouseWheel; }
};

struct QuitEvent : public Event {
    QuitEvent() { type = EventType::Quit; }
};

using EventHandler = std::function<void(const Event&)>;

class EventBus {
public:
    static EventBus& getInstance() {
        static EventBus instance;
        return instance;
    }

    void subscribe(EventType type, EventHandler handler) {
        size_t index = static_cast<size_t>(type);
        if (index < static_cast<size_t>(EventType::Count)) {
            subscribers[index].push_back(handler);
        }
    }

    void publish(const Event& event) {
        size_t index = static_cast<size_t>(event.type);
        if (index < static_cast<size_t>(EventType::Count)) {
            for (auto& handler : subscribers[index]) {
                handler(event);
            }
        }
    }

private:
    // ⚡ Bolt: Replaced std::unordered_map with a flat std::array indexed by EventType.
    // This turns a hashing operation with potential cache misses into an O(1) contiguous
    // array lookup, significantly improving performance on the hot path of the event bus.
    std::array<std::vector<EventHandler>, static_cast<size_t>(EventType::Count)> subscribers;
};
