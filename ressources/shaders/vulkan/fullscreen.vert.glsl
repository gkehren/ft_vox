#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 vUV;
void main()
{
    // Scene already uses a negative-Y viewport (OpenGL-style projection).
    // Do not flip UV here — a second flip puts the world upside-down and makes
    // camera look / WASD feel inverted relative to the picture.
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
