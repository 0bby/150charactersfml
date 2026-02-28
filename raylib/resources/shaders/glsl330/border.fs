#version 330

in vec2 fragTexCoord;

uniform float time;
uniform float proximity;

out vec4 finalColor;

void main()
{
    vec2 uv = fragTexCoord;
    float wx = uv.x * 10.0;

    // --- UV warping for swirl ---
    float warpX = wx + 0.3 * sin(uv.y * 4.0 + time * 0.7) + 0.15 * cos(uv.y * 7.0 - time * 0.4);
    float warpY = uv.y + 0.08 * sin(wx * 3.0 + time * 0.5) + 0.05 * cos(wx * 5.0 - time * 0.8);

    // Noise layers
    float n1 = sin(warpX * 1.2 + warpY * 2.5 + time * 0.4) * 0.5 + 0.5;
    float n2 = sin(warpX * 2.8 - time * 0.6 + warpY * 5.0) * 0.5 + 0.5;
    float n3 = sin(warpX * 4.5 + warpY * 8.0 + time * 0.9 + cos(warpX * 2.0 + time * 0.3) * 1.5) * 0.5 + 0.5;
    float n4 = sin(warpY * 10.0 - time * 1.5 + warpX * 1.8 + sin(time * 0.7) * 2.0) * 0.5 + 0.5;
    float pattern = n1 * 0.35 + n2 * 0.30 + n3 * 0.20 + n4 * 0.15;
    pattern = smoothstep(0.2, 0.8, pattern);

    float swirl2 = sin(wx * 1.5 + uv.y * 3.0 - time * 0.35 + pattern * 3.0) * 0.5 + 0.5;
    pattern = mix(pattern, swirl2, 0.25);

    // === Fiery wispy tips at the top ===
    // Extra high-freq flickering layers that only matter near the top
    float tipWarp = uv.y * uv.y; // squared so it only kicks in high up
    float flicker1 = sin(warpX * 6.0 + uv.y * 15.0 - time * 3.5) * 0.5 + 0.5;
    float flicker2 = sin(warpX * 9.0 - uv.y * 20.0 + time * 4.2 + cos(wx * 3.0) * 2.0) * 0.5 + 0.5;
    float tipNoise = flicker1 * 0.6 + flicker2 * 0.4;
    tipNoise = smoothstep(0.35, 0.85, tipNoise);

    // Blend tips into pattern at the top — at bottom tipWarp≈0 so no effect
    pattern = mix(pattern, tipNoise, tipWarp * 0.7);

    // === Vertical fade: 1/x hyperbolic taper ===
    // At y=0: ~1.0 (solid). Falls off like 1/x — fast initial drop then long wispy tail.
    float t = uv.y * 5.0 + 1.0;  // remap so y=0→1, y=1→6
    float vertFade = 1.0 / t;     // hyperbolic: 1/1=1.0, 1/2=0.5, 1/3=0.33, 1/6=0.17

    // Let the pattern punch wispy tendrils higher into the faded region
    float wispMask = pattern * 0.4 / (uv.y * 3.0 + 1.0);
    vertFade = max(vertFade, wispMask);

    // === Colors: saturated and punchy at base, hot at tips ===
    vec3 colCore   = vec3(0.4, 0.0, 0.7);     // saturated purple
    vec3 colMid    = vec3(0.75, 0.05, 0.65);   // hot magenta
    vec3 colBright = vec3(1.0, 0.3, 0.9);      // bright pink
    vec3 colTip    = vec3(1.0, 0.7, 1.0);      // white-pink hot tips

    // Height-driven color: saturated at base, brighter toward tips
    vec3 color;
    if (uv.y < 0.3)
        color = mix(colCore, colMid, uv.y / 0.3);
    else if (uv.y < 0.6)
        color = mix(colMid, colBright, (uv.y - 0.3) / 0.3);
    else
        color = mix(colBright, colTip, (uv.y - 0.6) / 0.4);

    // Boost color intensity at the base
    float baseSaturate = 1.0 - uv.y * 0.8;
    color *= 1.0 + baseSaturate * 0.6;

    // Hot streaks where pattern peaks
    float streak = smoothstep(0.7, 0.95, pattern);
    color = mix(color, colTip, streak * 0.4);

    // === Alpha: solid base, wispy top ===
    float baseAlpha = (1.0 - uv.y) * (1.0 - uv.y); // quadratic — very strong at base
    float alpha = (vertFade * (0.6 + pattern * 0.4) * 0.85 + baseAlpha * 0.35) * proximity;
    alpha = clamp(alpha, 0.0, 1.0);

    finalColor = vec4(color, alpha);
}
