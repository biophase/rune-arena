#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float uFlashAmount;

void main() {
    vec4 texel = texture(texture0, fragTexCoord) * fragColor * colDiffuse;
    texel.rgb = mix(texel.rgb, vec3(1.0), clamp(uFlashAmount, 0.0, 1.0));
    finalColor = texel;
}
