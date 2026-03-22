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
uniform vec2 uArenaCenter;
uniform float uArenaRadius;

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
    if (UnsafeCoverage() > 0.5) {
        float luma = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        texel.rgb = vec3(luma);
    }
    finalColor = texel;
}
