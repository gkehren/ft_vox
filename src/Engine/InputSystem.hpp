// src/Engine/InputSystem.hpp
#pragma once
#include <SDL3/SDL.h>
#include "EventBus.hpp"

class InputSystem {
public:
    InputSystem();
    ~InputSystem();

    void update();
    bool isMouseCaptured() const { return mouseCaptured; }
    void setMouseCaptured(bool captured, SDL_Window* window);

private:
    bool mouseCaptured;
};
