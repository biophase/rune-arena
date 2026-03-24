#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float uScreenHeight;
uniform vec2 uCameraTarget;
uniform vec2 uCameraOffset;
uniform float uCameraZoom;
uniform vec2 uZoneCenter;
uniform float uZoneRadius;
uniform vec4 uZoneFillRectPx;

vec2 ScreenToWorld(vec2 screenPos) {
    return (screenPos - uCameraOffset) / max(uCameraZoom, 0.0001) + uCameraTarget;
}

vec2 PositiveMod(vec2 value, vec2 modulus) {
    return mod(mod(value, modulus) + modulus, modulus);
}

void main() {
    vec2 screenPos = vec2(gl_FragCoord.x, uScreenHeight - gl_FragCoord.y);
    vec2 worldPos = ScreenToWorld(screenPos);
    if (uZoneRadius <= 0.0 || distance(worldPos, uZoneCenter) <= uZoneRadius) {
        finalColor = vec4(0.0);
        return;
    }

    vec2 atlasSize = vec2(textureSize(texture0, 0));
    vec2 localPx = PositiveMod(floor(worldPos), uZoneFillRectPx.zw);
    vec2 atlasPx = uZoneFillRectPx.xy + localPx + vec2(0.5);
    vec2 fillUv = atlasPx / atlasSize;
    finalColor = texture(texture0, fillUv) * fragColor * colDiffuse;
}
