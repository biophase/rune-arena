#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec4 uFillSourceRectPx;
uniform vec4 uMaskSourceRectPx;

void main() {
    vec4 texel = texture(texture0, fragTexCoord) * fragColor * colDiffuse;
    vec2 atlasSize = vec2(textureSize(texture0, 0));
    vec2 atlasPx = fragTexCoord * atlasSize;
    vec2 localUv = clamp((atlasPx - uFillSourceRectPx.xy) / max(uFillSourceRectPx.zw, vec2(1.0)), 0.0, 1.0);
    vec2 maskUv = (uMaskSourceRectPx.xy + localUv * uMaskSourceRectPx.zw) / atlasSize;
    vec4 maskTexel = texture(texture0, maskUv);
    float coverage = max(maskTexel.g, maskTexel.a);
    texel.a *= coverage;
    finalColor = texel;
}
