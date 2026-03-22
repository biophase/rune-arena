#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform int uRevealCount;
uniform vec4 uRevealData[8];
uniform float uScreenHeight;
uniform float uInsideAlpha;
uniform vec4 uSourceRectPx;
uniform vec2 uCameraTarget;
uniform vec2 uCameraOffset;
uniform float uCameraZoom;
uniform vec2 uArenaCenter;
uniform float uArenaRadius;

float Bayer4(vec2 p) {
    int x = int(mod(p.x, 4.0));
    int y = int(mod(p.y, 4.0));
    int idx = x + y * 4;
    float thresholds[16] = float[16](
        0.0 / 16.0, 8.0 / 16.0, 2.0 / 16.0, 10.0 / 16.0,
        12.0 / 16.0, 4.0 / 16.0, 14.0 / 16.0, 6.0 / 16.0,
        3.0 / 16.0, 11.0 / 16.0, 1.0 / 16.0, 9.0 / 16.0,
        15.0 / 16.0, 7.0 / 16.0, 13.0 / 16.0, 5.0 / 16.0
    );
    return thresholds[idx];
}

float UnsafeCoverage() {
    if (uArenaRadius <= 0.0 || uCameraZoom <= 0.0) {
        return 0.0;
    }
    vec2 screenPos = vec2(gl_FragCoord.x, uScreenHeight - gl_FragCoord.y);
    vec2 worldPos = (screenPos - uCameraOffset) / uCameraZoom + uCameraTarget;
    return step(uArenaRadius, distance(worldPos, uArenaCenter));
}

void main() {
    vec4 texel = texture(texture0, fragTexCoord) * fragColor * colDiffuse;
    vec2 screenPos = vec2(gl_FragCoord.x, uScreenHeight - gl_FragCoord.y);
    vec2 atlasSize = vec2(textureSize(texture0, 0));
    vec2 atlasPx = fragTexCoord * atlasSize;
    vec2 localSpritePx = floor(atlasPx - uSourceRectPx.xy);

    float reveal = 0.0;
    for (int i = 0; i < 8; ++i) {
        if (i >= uRevealCount) {
            break;
        }
        vec4 data = uRevealData[i];
        float distPx = distance(screenPos, data.xy);
        float contribution = 1.0 - smoothstep(data.z, data.w, distPx);
        reveal = max(reveal, contribution);
    }

    if (reveal > 0.0) {
        float alphaMul = 1.0;
        if (reveal >= 0.999) {
            alphaMul = uInsideAlpha;
        } else {
            float dither = Bayer4(localSpritePx);
            alphaMul = (reveal > dither) ? uInsideAlpha : 1.0;
        }
        texel.a *= alphaMul;
    }

    if (UnsafeCoverage() > 0.5) {
        float luma = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        texel.rgb = vec3(luma);
    }

    finalColor = texel;
}
