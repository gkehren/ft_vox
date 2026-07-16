// Shared FrameUBO layout — must match src/Renderer/FrameUBO.hpp (sizeof 528).
// Include by pasting into each shader (GLSL has no project-wide #include pipeline yet).

// layout(set = 0, binding = 0) uniform FrameUBO {
//     mat4 view;
//     mat4 projection;
//     mat4 cascadeMatrix0;
//     mat4 cascadeMatrix1;
//     mat4 cascadeMatrix2;
//     vec4 viewPos;
//     vec4 lightDirection;
//     vec4 fogColor;
//     vec4 fogParams;
//     vec4 lightParams;
//     vec4 visualParams;
//     vec4 sunDir;
//     vec4 moonDir;
//     vec4 skyParams;
//     vec4 cascadeSplits;
//     vec4 moonAmbient;
//     vec4 lightingParams;
//     vec4 waterParams;
// } frame;
