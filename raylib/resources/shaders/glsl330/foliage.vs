#version 330

// Input vertex attributes
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in vec4 vertexTangent;

// Instancing: per-instance model matrix (mat4 = 4 x vec4 attributes)
in mat4 instanceTransform;

// Input uniform values
uniform mat4 mvp;
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
    // instanceTransform already contains model.transform * envPiece transform
    // (baked on CPU side), so no need for matModel uniform.
    vec4 worldPos = instanceTransform * vec4(vertexPosition, 1.0);

    // --- Wind sway ---
    // Height factor: sway increases from base (y=0) to top
    // Use the local vertex Y relative to instance origin for height gradient
    float localY = worldPos.y - instanceTransform[3].y;
    float heightFactor = clamp(localY / 15.0, 0.0, 1.0);
    heightFactor = heightFactor * heightFactor; // quadratic falloff - base stays still

    // Per-instance phase offset from world position (so each clump sways differently)
    float phase = instanceTransform[3].x * 0.37 + instanceTransform[3].z * 0.53;

    // Primary wind sway (slow, large)
    float swayX = sin(time * 1.2 + phase) * 1.8 * heightFactor;
    float swayZ = cos(time * 0.9 + phase * 1.3) * 1.2 * heightFactor;

    // Secondary gust (faster, smaller, adds organic feel)
    swayX += sin(time * 2.8 + phase * 2.1) * 0.5 * heightFactor;
    swayZ += cos(time * 3.1 + phase * 1.7) * 0.4 * heightFactor;

    worldPos.x += swayX;
    worldPos.z += swayZ;

    fragPosition = worldPos.xyz;
    fragTexCoord = vertexTexCoord;
    fragColor = vec4(1.0); // OBJ meshes lack vertex colors; default to white

    // --- Compute sway rotation for normals ---
    // The sway is essentially a bend: approximate the tilt at this height
    float dSwayX_dY = (2.0 * heightFactor / max(localY, 0.1)) * swayX;
    float dSwayZ_dY = (2.0 * heightFactor / max(localY, 0.1)) * swayZ;

    // Clamp derivatives to avoid extreme rotations at base
    dSwayX_dY = clamp(dSwayX_dY, -0.5, 0.5);
    dSwayZ_dY = clamp(dSwayZ_dY, -0.5, 0.5);

    // Build a small rotation: tilt around Z by -dSwayX_dY, tilt around X by dSwayZ_dY
    // Using small angle approximation for the rotation matrix
    mat3 swayRot = mat3(
        1.0,        0.0,       dSwayX_dY,
        0.0,        1.0,       dSwayZ_dY,
        -dSwayX_dY, -dSwayZ_dY, 1.0
    );

    // Normal matrix from instance transform (upper-left 3x3)
    mat3 normalMat3 = mat3(
        instanceTransform[0].xyz,
        instanceTransform[1].xyz,
        instanceTransform[2].xyz
    );

    vec3 N = normalize(swayRot * normalMat3 * vertexNormal);
    fragNormal = N;

    fragPosLightSpace = lightVP * worldPos;

    // Compute TBN matrix for normal mapping (with sway rotation applied)
    vec3 T = normalize(swayRot * normalMat3 * vertexTangent.xyz);
    T = normalize(T - dot(T, N) * N); // re-orthogonalize
    vec3 B = cross(N, T) * vertexTangent.w;
    fragTBN = mat3(T, B, N);

    // mvp = projection * view (for instanced draws, matModel is identity)
    // worldPos is already in world space, so this is correct
    gl_Position = mvp * worldPos;
}
