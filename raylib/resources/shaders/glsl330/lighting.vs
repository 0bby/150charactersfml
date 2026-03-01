#version 330

// Input vertex attributes
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in vec4 vertexTangent;

// Input uniform values
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
uniform mat4 lightVP;

// Output vertex attributes (to fragment shader)
out vec3 fragPosition;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
out vec4 fragPosLightSpace;
out mat3 fragTBN;

void main()
{
    // Send vertex attributes to fragment shader
    vec4 worldPos = matModel*vec4(vertexPosition, 1.0);
    fragPosition = worldPos.xyz;
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal*vec4(vertexNormal, 1.0)));
    fragPosLightSpace = lightVP * worldPos;

    // Compute TBN matrix for normal mapping
    vec3 T = normalize(vec3(matModel*vec4(vertexTangent.xyz, 0.0)));
    vec3 N = fragNormal;
    T = normalize(T - dot(T, N)*N); // re-orthogonalize
    vec3 B = cross(N, T) * vertexTangent.w;
    fragTBN = mat3(T, B, N);

    // Calculate final vertex position
    gl_Position = mvp*vec4(vertexPosition, 1.0);
}
