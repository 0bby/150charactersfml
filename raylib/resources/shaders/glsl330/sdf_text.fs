#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

out vec4 finalColor;

void main()
{
    float d = texture(texture0, fragTexCoord).a;
    float aaf = fwidth(d);
    float alpha = smoothstep(0.5 - aaf, 0.5 + aaf, d);
    finalColor = vec4(fragColor.rgb * colDiffuse.rgb, fragColor.a * colDiffuse.a * alpha);
}
