#version 330

// Input vertex attributes (from vertex shader)
in vec3 fragPosition;
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;
in vec4 fragPosLightSpace;
in mat3 fragTBN;

// Input uniform values
uniform sampler2D texture0;      // BC (diffuse/albedo)
uniform sampler2D texture1;      // ORM (Occlusion, Roughness, Metallic)
uniform sampler2D shadowMap;
uniform sampler2D normalMap;
uniform int useNormalMap;
uniform vec4 colDiffuse;

// Output fragment color
out vec4 finalColor;

#define     MAX_LIGHTS              4
#define     LIGHT_DIRECTIONAL       0
#define     LIGHT_POINT             1

struct Light {
    int enabled;
    int type;
    vec3 position;
    vec3 target;
    vec4 color;
};

// Input lighting values
uniform Light lights[MAX_LIGHTS];
uniform vec4 ambient;
uniform vec3 viewPos;
uniform vec3 fogColor;
uniform float fogDensity;
uniform int shadowDebug; // 0=off, 1=shadow factor, 2=light depth, 3=light UV, 4=sampled depth
uniform int noShadow;    // 1=skip shadows + apply own gamma (for intro/offscreen renders)

float ShadowCalculation(vec4 posLightSpace)
{
    vec3 projCoords = posLightSpace.xyz / posLightSpace.w * 0.5 + 0.5;
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
        return 1.0;
    float currentDepth = projCoords.z;
    // PCF 3x3
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += (currentDepth - 0.005) > pcfDepth ? 0.3 : 1.0;
        }
    }
    return shadow / 9.0;
}

void main()
{
    // Texel color fetching from texture sampler
    vec4 texelColor = texture(texture0, fragTexCoord);
    vec3 orm = texture(texture1, fragTexCoord).rgb; // R=AO, G=Roughness, B=Metallic
    float ao = orm.r;
    float roughness = orm.g;

    vec3 lightDot = vec3(0.0);
    vec3 normal;
    if (useNormalMap == 1) {
        normal = texture(normalMap, fragTexCoord).rgb * 2.0 - 1.0;
        normal = normalize(fragTBN * normal);
    } else {
        normal = normalize(fragNormal);
    }
    vec3 viewD = normalize(viewPos - fragPosition);
    vec3 specular = vec3(0.0);

    float shadow = (noShadow == 1) ? 1.0 : ShadowCalculation(fragPosLightSpace);

    // Debug visualizations
    if (shadowDebug == 1) {
        // Shadow factor: white = lit, dark = shadowed
        finalColor = vec4(vec3(shadow), 1.0);
        return;
    }
    if (shadowDebug == 2) {
        // Light-space depth (currentDepth)
        vec3 pc = fragPosLightSpace.xyz / fragPosLightSpace.w * 0.5 + 0.5;
        finalColor = vec4(vec3(pc.z), 1.0);
        return;
    }
    if (shadowDebug == 3) {
        // Light-space UV coords: R=U, G=V, B=0
        vec3 pc = fragPosLightSpace.xyz / fragPosLightSpace.w * 0.5 + 0.5;
        finalColor = vec4(pc.xy, 0.0, 1.0);
        return;
    }
    if (shadowDebug == 4) {
        // Shadow map sampled depth (what the light "sees")
        vec3 pc = fragPosLightSpace.xyz / fragPosLightSpace.w * 0.5 + 0.5;
        float d = texture(shadowMap, pc.xy).r;
        finalColor = vec4(vec3(d), 1.0);
        return;
    }

    vec4 tint = colDiffuse*fragColor;

    for (int i = 0; i < MAX_LIGHTS; i++)
    {
        if (lights[i].enabled == 1)
        {
            vec3 light = vec3(0.0);

            if (lights[i].type == LIGHT_DIRECTIONAL)
            {
                light = -normalize(lights[i].target - lights[i].position);
            }

            if (lights[i].type == LIGHT_POINT)
            {
                light = normalize(lights[i].position - fragPosition);
            }

            float NdotL = max(dot(normal, light), 0.0);

            // Apply shadow only to directional light (light 0)
            float shadowFactor = (i == 0 && lights[i].type == LIGHT_DIRECTIONAL) ? shadow : 1.0;
            lightDot += lights[i].color.rgb * NdotL * shadowFactor;

            float specCo = 0.0;
            if (NdotL > 0.0) {
                float specPower = mix(64.0, 8.0, roughness);
                float specScale = mix(0.5, 0.05, roughness);
                specCo = pow(max(0.0, dot(viewD, reflect(-light, normal))), specPower) * specScale * shadowFactor;
            }
            specular += specCo;
        }
    }

    finalColor = (texelColor*((tint + vec4(specular, 1.0))*vec4(lightDot, 1.0)));
    finalColor += texelColor*(ambient/10.0)*tint*ao;

    // Gamma correction for offscreen renders that skip the SSAO/tonemap pass
    if (noShadow == 1) finalColor = pow(finalColor, vec4(1.0/2.2));

    // Distance fog
    float dist = length(viewPos - fragPosition);
    float fogFactor = 1.0 - exp(-fogDensity * dist);
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    finalColor = mix(finalColor, vec4(fogColor, 1.0), fogFactor);
}
