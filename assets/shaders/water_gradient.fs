#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float uScreenHeight;
uniform vec4 uGradientStart;
uniform vec4 uGradientEnd;

void main() {
    vec4 texel = texture(texture0, fragTexCoord) * fragColor * colDiffuse;
    float t = clamp((uScreenHeight - gl_FragCoord.y) / max(uScreenHeight, 1.0), 0.0, 1.0);
    vec4 gradient = mix(uGradientStart, uGradientEnd, t);
    texel.rgb = mix(texel.rgb, gradient.rgb, gradient.a);
    finalColor = texel;
}
