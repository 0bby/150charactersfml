#version 330

in vec3 vertexPosition;

uniform mat4 matModel;
uniform mat4 matView;
uniform mat4 matProjection;
uniform float time;

void main()
{
    vec4 worldPos = matModel * vec4(vertexPosition, 1.0);

    // --- Wind sway (must match foliage.vs exactly) ---
    float localY = worldPos.y - matModel[3].y;
    float h = clamp(localY / 20.0, 0.0, 1.0);
    float bendFactor = h * h * h;

    float phase = matModel[3].x * 0.37 + matModel[3].z * 0.53;

    float swayX = sin(time * 0.8 + phase) * 0.9 * bendFactor;
    float swayZ = cos(time * 0.6 + phase * 1.3) * 0.6 * bendFactor;
    swayX += sin(time * 1.6 + phase * 2.1) * 0.3 * bendFactor;
    swayZ += cos(time * 1.9 + phase * 1.7) * 0.25 * bendFactor;

    // Leaf flutter
    float tipFactor = h * h;
    float leafPhase = vertexPosition.x * 1.7 + vertexPosition.z * 2.3;
    swayX += sin(time * 4.5 + leafPhase + phase) * 0.15 * tipFactor;
    swayZ += cos(time * 5.2 + leafPhase + phase * 0.8) * 0.12 * tipFactor;

    worldPos.x += swayX;
    worldPos.z += swayZ;

    gl_Position = matProjection * matView * worldPos;
}
