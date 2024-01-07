#version 410 core
out vec4 FragColor;

in vec3 Normal;
in vec3 FragPos;
in vec2 TexCoord;

uniform sampler2D textureSampler;
uniform vec3 lightPos;
uniform vec3 viewPos;

void main()
{
    // Ambient
    // float ambientStrength = 0.1;
    // vec3 ambient = ambientStrength * vec3(1.0, 1.0, 1.0);

    // Diffuse
    // vec3 norm = normalize(Normal);
    // vec3 lightDir = normalize(lightPos - FragPos);
    // float diff = max(dot(norm, lightDir), 0.0);
    // vec3 diffuse = diff * vec3(1.0, 1.0, 1.0);

    // vec3 result = (ambient + diffuse) * vec3(texture(textureSampler, TexCoord));
    FragColor = texture(textureSampler, TexCoord);
}
