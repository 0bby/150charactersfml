#version 330

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;      // scene color
uniform sampler2D texture1;      // scene depth
uniform vec2 resolution;
uniform float near;              // camera near plane
uniform float far;               // camera far plane

// Linearize depth from depth buffer [0,1] to view-space distance
float linearizeDepth(float d) {
    return near * far / (far - d * (far - near));
}

void main() {
    vec4 sceneColor = texture(texture0, fragTexCoord);
    float depth = texture(texture1, fragTexCoord).r;
    float linDepth = linearizeDepth(depth);

    // Skip SSAO for sky / far pixels
    if (depth > 0.999) {
        finalColor = sceneColor;
        return;
    }

    // Sample offsets in screen space (small radius for performance)
    float radius = 3.0 / resolution.y;  // ~3 pixels
    float occlusion = 0.0;
    const int samples = 8;
    // Fixed sample directions (no noise texture needed)
    vec2 offsets[8] = vec2[](
        vec2( 1.0,  0.0), vec2(-1.0,  0.0),
        vec2( 0.0,  1.0), vec2( 0.0, -1.0),
        vec2( 0.707,  0.707), vec2(-0.707,  0.707),
        vec2( 0.707, -0.707), vec2(-0.707, -0.707)
    );

    for (int i = 0; i < samples; i++) {
        vec2 sampleUV = fragTexCoord + offsets[i] * radius;
        float sampleDepth = linearizeDepth(texture(texture1, sampleUV).r);

        // If neighbor is closer to camera, this pixel is occluded
        float diff = linDepth - sampleDepth;
        // Only count occlusion within a range to avoid halos
        if (diff > 0.1 && diff < 8.0) {
            occlusion += 1.0;
        }
    }

    occlusion = occlusion / float(samples);
    float ao = 1.0 - occlusion * 0.6;  // strength factor

    vec3 color = sceneColor.rgb * ao;

    // ACES filmic tonemap (Narkowicz fit) + gamma
    color *= 20; // exposure
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    color = clamp((color*(a*color + b)) / (color*(c*color + d) + e), 0.0, 1.0);
    color = pow(color, vec3(1.0/2.2));

    finalColor = vec4(color, sceneColor.a);
}
