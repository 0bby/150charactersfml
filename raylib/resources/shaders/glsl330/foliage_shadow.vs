#version 330

in vec3 vertexPosition;

// Instancing: per-instance model matrix
in mat4 instanceTransform;

uniform mat4 mvp;
uniform float time;

void main()
{
    // instanceTransform already contains model.transform * envPiece transform
    // (baked on CPU side), so no need for matModel uniform.
    vec4 worldPos = instanceTransform * vec4(vertexPosition, 1.0);

    // --- Wind sway (must match foliage.vs exactly) ---
    float localY = worldPos.y - instanceTransform[3].y;
    float heightFactor = clamp(localY / 15.0, 0.0, 1.0);
    heightFactor = heightFactor * heightFactor;

    float phase = instanceTransform[3].x * 0.37 + instanceTransform[3].z * 0.53;

    float swayX = sin(time * 1.2 + phase) * 1.8 * heightFactor;
    float swayZ = cos(time * 0.9 + phase * 1.3) * 1.2 * heightFactor;
    swayX += sin(time * 2.8 + phase * 2.1) * 0.5 * heightFactor;
    swayZ += cos(time * 3.1 + phase * 1.7) * 0.4 * heightFactor;

    worldPos.x += swayX;
    worldPos.z += swayZ;

    gl_Position = mvp * worldPos;
}
