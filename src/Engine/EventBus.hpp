// src/Engine/EventBus.hpp
#pragma once
#include <functional>
#include <vector>
#include <memory>
#include <unordered_map>
#include <SDL3/SDL.h>

enum class EventType {
    WindowResize,
    KeyPress,
    KeyRelease,
    MouseMotion,
    MouseButtonPress,
    MouseButtonRelease,
    Quit
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
        subscribers[type].push_back(handler);
    }

    void publish(const Event& event) {
        if (subscribers.find(event.type) != subscribers.end()) {
            for (auto& handler : subscribers[event.type]) {
                handler(event);
            }
        }
    }

private:
    std::unordered_map<EventType, std::vector<EventHandler>> subscribers;
};
