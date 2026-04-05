// src/Engine/InputSystem.hpp
#pragma once
#include <SDL3/SDL.h>
#include <unordered_map>
#include "EventBus.hpp"

class InputSystem {
public:
    InputSystem();
    ~InputSystem();

    void update();
    bool isKeyPressed(SDL_Keycode key) const;
    bool isMouseCaptured() const { return mouseCaptured; }
    void setMouseCaptured(bool captured, SDL_Window* window);

private:
    bool mouseCaptured;
    std::unordered_map<SDL_Keycode, bool> keyStates;
};
