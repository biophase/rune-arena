#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec4 uFrameRectPx;
uniform float uTime;
uniform float uSwayStrengthPixels;
uniform float uSwaySpeed;
uniform float uPhaseOffset;
uniform float uGradientStart;

float ComputeWindOffsetPx(vec2 localUv) {
    float clampedY = clamp(localUv.y, 0.0, 1.0);
    float topGradient = 1.0 - smoothstep(uGradientStart, 1.0, clampedY);
    float wave = sin(uTime * uSwaySpeed + uPhaseOffset + clampedY * 5.5);
    return round(wave * uSwayStrengthPixels * topGradient);
}

void main() {
    vec2 atlasSize = vec2(textureSize(texture0, 0));
    vec2 localPx = fragTexCoord * atlasSize - uFrameRectPx.xy;
    vec2 frameSize = uFrameRectPx.zw;
    float windOffsetPx = ComputeWindOffsetPx((localPx + vec2(0.5)) / frameSize);
    vec2 samplePx = localPx;
    samplePx.x -= windOffsetPx;

    if (samplePx.x < 0.0 || samplePx.y < 0.0 || samplePx.x >= frameSize.x || samplePx.y >= frameSize.y) {
        finalColor = vec4(0.0);
        return;
    }

    vec2 sampleUv = (uFrameRectPx.xy + samplePx + vec2(0.5)) / atlasSize;
    finalColor = texture(texture0, sampleUv) * fragColor * colDiffuse;
}
