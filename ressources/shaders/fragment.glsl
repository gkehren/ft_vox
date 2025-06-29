#version 460 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;
in float TextureIndex;
in float UseBiomeColor;
in vec3 BiomeColor;

uniform sampler2DArray textureArray;
uniform vec3 sunDirection;
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
    // Récupération de la couleur de la texture depuis le texture array
    vec4 texColor = texture(textureArray, vec3(TexCoord, TextureIndex));

    // Réduire le seuil alpha pour permettre aux textures partiellement transparentes de s'afficher
    // Ne pas rejeter les fragments complètement transparents pour les types spécifiques
    if (texColor.a < 0.01) discard;

    vec3 color = texColor.rgb;

    // Apply biome coloring to grayscale parts of textures
    if (UseBiomeColor > 0.5) {
        // Calcul de la luminance (niveau de gris)
        float luminance = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));

        // Déterminer si le pixel est gris (R≈G≈B)
        // Méthode améliorée pour détecter les parties grises
        float maxChannel = max(max(texColor.r, texColor.g), texColor.b);
        float minChannel = min(min(texColor.r, texColor.g), texColor.b);
        float colorfulnessRatio = (maxChannel - minChannel) / max(0.001, maxChannel);

        // Plus le ratio est bas, plus la couleur est "grise"
        float grayscaleFactor = 1.0 - min(colorfulnessRatio * 4.0, 1.0);

        // Mélanger la texture originale avec la couleur du biome
        // appliquée uniquement aux parties grises
        vec3 coloredPart = luminance * BiomeColor * 1.5; // Intensifier légèrement
        vec3 biomeColored = mix(texColor.rgb, coloredPart, grayscaleFactor * 0.8);
        color = biomeColored;
    }

    vec3 norm = normalize(Normal);

    // Direction de la lumière
    vec3 lightDir = normalize(sunDirection);

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