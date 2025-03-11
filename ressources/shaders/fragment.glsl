#version 460 core
out vec4 FragColor;

in vec3 Normal;
in vec3 FragPos;
in vec2 TexCoord;

uniform sampler2D textureSampler;
uniform vec3 lightPos;
uniform vec3 viewPos;

// Paramètres du fog
uniform float fogStart = 160.0;
uniform float fogEnd = 300.0;
uniform vec3 fogColor = vec3(0.75, 0.85, 1.0);
uniform float fogDensity = 0.8;

// Paramètres visuels
uniform float ambientStrength = 0.2;
uniform float diffuseIntensity = 0.7;
uniform float lightLevels = 5.0;
uniform float saturationLevel = 1.2;
uniform float colorBoost = 1.0;
uniform float gamma = 1.8;

void main()
{
    // Récupération de la couleur de la texture
    vec4 texColor = texture(textureSampler, TexCoord);
    if(texColor.a < 0.1) discard; // Optional: transparency handling

    // Augmenter la saturation des couleurs de la texture
    vec3 color = texColor.rgb;
    float luminance = 0.299 * color.r + 0.587 * color.g + 0.114 * color.b;
    vec3 saturatedColor = mix(vec3(luminance), color, saturationLevel);
    color = saturatedColor;

    vec3 norm = normalize(Normal);

    // Direction de la lumière
    vec3 lightDir = normalize(lightPos - FragPos);

    // Éclairage ambient
    vec3 ambient = ambientStrength * color;

    // Éclairage diffus
    float diff = max(dot(norm, lightDir), 0.0) * diffuseIntensity;

    // "Blocky lighting" de Minecraft
    diff = floor(diff * lightLevels) / lightLevels;
    vec3 diffuse = diff * color;

    // Accentuer davantage l'éclairage directionnel basé sur la face
    float topLight = 0.0;
    if(norm.y > 0.9) { // Face supérieure
        topLight = 0.3;
    } else if(norm.y < -0.9) { // Face inférieure
        topLight = -0.15;
    } else if(abs(norm.x) > 0.9) { // Faces latérales (est/ouest)
        topLight = 0.05;
    }

    // Résultat combiné
    vec3 result = ambient + diffuse + topLight * color;

    // Boost de couleur
    result *= colorBoost;

    // Éviter la saturation
    result = min(result, vec3(1.0));

    // Correction gamma
    result = pow(result, vec3(1.0/gamma));

    // Brouillard
    float dist = length(FragPos - viewPos);
    float fogFactor = clamp((fogEnd - dist) / (fogEnd - fogStart), 0.0, 1.0);

    // Réduire l'effet du brouillard en fonction de la densité
    fogFactor = pow(fogFactor, fogDensity);

    result = mix(fogColor, result, fogFactor);

    FragColor = vec4(result, texColor.a);
}