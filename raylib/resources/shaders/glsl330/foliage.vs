#version 330

// Input vertex attributes
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in vec4 vertexTangent;

// Input uniform values
uniform mat4 matModel;
uniform mat4 matNormal;
uniform mat4 matView;
uniform mat4 matProjection;
uniform mat4 lightVP;
uniform float time;

// Output vertex attributes (to fragment shader)
out vec3 fragPosition;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
out vec4 fragPosLightSpace;
out mat3 fragTBN;

void main()
{
    vec4 worldPos = matModel * vec4(vertexPosition, 1.0);

    // --- Wind sway ---
    // Local height relative to model origin (pivot at base)
    float localY = worldPos.y - matModel[3].y;
    // Normalize height: 0 at base, 1 at ~20 units up
    float h = clamp(localY / 20.0, 0.0, 1.0);
    // Cubic falloff: base stays completely anchored, tips get full sway
    float bendFactor = h * h * h;

    // Per-piece phase offset from world position (each clump sways differently)
    float phase = matModel[3].x * 0.37 + matModel[3].z * 0.53;

    // Primary trunk/branch sway (slow, gentle)
    float swayX = sin(time * 0.8 + phase) * 0.9 * bendFactor;
    float swayZ = cos(time * 0.6 + phase * 1.3) * 0.6 * bendFactor;

    // Secondary gust (medium speed, adds organic variation)
    swayX += sin(time * 1.6 + phase * 2.1) * 0.3 * bendFactor;
    swayZ += cos(time * 1.9 + phase * 1.7) * 0.25 * bendFactor;

    // Leaf flutter: high-frequency, small amplitude, only at the tips
    // Uses vertex position as extra phase so individual leaves shimmer
    float tipFactor = h * h; // starts halfway up, strongest at tip
    float leafPhase = vertexPosition.x * 1.7 + vertexPosition.z * 2.3;
    swayX += sin(time * 4.5 + leafPhase + phase) * 0.15 * tipFactor;
    swayZ += cos(time * 5.2 + leafPhase + phase * 0.8) * 0.12 * tipFactor;

    worldPos.x += swayX;
    worldPos.z += swayZ;

    fragPosition = worldPos.xyz;
    fragTexCoord = vertexTexCoord;
    fragColor = vec4(1.0); // OBJ meshes lack vertex colors; default to white

    // --- Compute sway rotation for normals ---
    // Approximate the tilt from bending at this height
    float dSwayX_dY = (3.0 * h * h / max(localY, 0.5)) * swayX;
    float dSwayZ_dY = (3.0 * h * h / max(localY, 0.5)) * swayZ;

    // Clamp derivatives to avoid extreme rotations
    dSwayX_dY = clamp(dSwayX_dY, -0.3, 0.3);
    dSwayZ_dY = clamp(dSwayZ_dY, -0.3, 0.3);

    // Small rotation from sway (small angle approximation)
    mat3 swayRot = mat3(
        1.0,        0.0,       dSwayX_dY,
        0.0,        1.0,       dSwayZ_dY,
        -dSwayX_dY, -dSwayZ_dY, 1.0
    );

    // Normal matrix from matNormal uniform
    mat3 normalMat3 = mat3(matNormal);

    vec3 N = normalize(swayRot * normalMat3 * vertexNormal);
    fragNormal = N;

    fragPosLightSpace = lightVP * worldPos;

    // Compute TBN matrix for normal mapping (with sway rotation applied)
    vec3 T = normalize(swayRot * normalMat3 * vertexTangent.xyz);
    T = normalize(T - dot(T, N) * N); // re-orthogonalize
    vec3 B = cross(N, T) * vertexTangent.w;
    fragTBN = mat3(T, B, N);

    gl_Position = matProjection * matView * worldPos;
}
