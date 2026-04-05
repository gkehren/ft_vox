// src/Engine/InputSystem.cpp
#include "InputSystem.hpp"
#include <imgui/imgui.h>
#include <imgui/imgui_impl_sdl3.h>

InputSystem::InputSystem() : mouseCaptured(true) {}

InputSystem::~InputSystem() {}

void InputSystem::update() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);

        if (event.type == SDL_EVENT_QUIT) {
            EventBus::getInstance().publish(QuitEvent());
        }
        else if (event.type == SDL_EVENT_KEY_DOWN && !ImGui::GetIO().WantCaptureKeyboard) {
            keyStates[event.key.key] = true;
            if (event.key.repeat == 0) {
                EventBus::getInstance().publish(KeyEvent(EventType::KeyPress, event.key.key));
            }
        }
        else if (event.type == SDL_EVENT_KEY_UP && !ImGui::GetIO().WantCaptureKeyboard) {
            keyStates[event.key.key] = false;
            EventBus::getInstance().publish(KeyEvent(EventType::KeyRelease, event.key.key));
        }
        else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
            EventBus::getInstance().publish(WindowResizeEvent(event.window.data1, event.window.data2));
        }
        else if (event.type == SDL_EVENT_MOUSE_MOTION && !ImGui::GetIO().WantCaptureMouse) {
            EventBus::getInstance().publish(MouseEvent(EventType::MouseMotion, (int)event.motion.x, (int)event.motion.y, (int)event.motion.xrel, (int)event.motion.yrel));
        }
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !ImGui::GetIO().WantCaptureMouse) {
            EventBus::getInstance().publish(MouseEvent(EventType::MouseButtonPress, (int)event.button.x, (int)event.button.y, 0, 0, event.button.button));
        }
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && !ImGui::GetIO().WantCaptureMouse) {
            EventBus::getInstance().publish(MouseEvent(EventType::MouseButtonRelease, (int)event.button.x, (int)event.button.y, 0, 0, event.button.button));
        }
        else if (event.type == SDL_EVENT_MOUSE_WHEEL && !ImGui::GetIO().WantCaptureMouse) {
            EventBus::getInstance().publish(MouseWheelEvent(event.wheel.x, event.wheel.y));
        }
    }
}

bool InputSystem::isKeyPressed(SDL_Keycode key) const {
    auto it = keyStates.find(key);
    return it != keyStates.end() ? it->second : false;
}

void InputSystem::setMouseCaptured(bool captured, SDL_Window* window) {
    mouseCaptured = captured;
    SDL_SetWindowRelativeMouseMode(window, captured);
}
