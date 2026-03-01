#version 330

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;

uniform float exposure;
uniform float contrast;
uniform float saturation;
uniform float temperature;
uniform float vignetteStrength;
uniform float vignetteSoftness;
uniform vec3 lift;
uniform vec3 gain;

void main()
{
    vec3 color = texture(texture0, fragTexCoord).rgb;

    // Exposure
    color *= exposure;

    // Contrast (pivot around 0.5)
    color = (color - 0.5) * contrast + 0.5;

    // Saturation (Rec.709 luma)
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, saturation);

    // Temperature (warm/cool shift)
    color.r += temperature * 0.02;
    color.b -= temperature * 0.02;

    // Lift/Gain (shadow tint + highlight tint)
    color = gain * color + lift * (1.0 - color);

    // Vignette
    if (vignetteStrength > 0.001) {
        vec2 uv = fragTexCoord - 0.5;
        float dist = length(uv);
        float vignette = smoothstep(vignetteSoftness, vignetteSoftness - vignetteStrength, dist);
        color *= vignette;
    }

    // Clamp to valid range
    color = clamp(color, 0.0, 1.0);

    finalColor = vec4(color, 1.0);
}
