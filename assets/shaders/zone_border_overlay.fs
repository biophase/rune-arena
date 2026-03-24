#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform float uScreenHeight;
uniform vec2 uCameraTarget;
uniform vec2 uCameraOffset;
uniform float uCameraZoom;
uniform vec2 uZoneCenter;
uniform float uZoneRadius;

vec2 ScreenToWorld(vec2 screenPos) {
    return (screenPos - uCameraOffset) / max(uCameraZoom, 0.0001) + uCameraTarget;
}

float Bayer4A(vec2 p) {
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

float Bayer4B(vec2 p) {
    return Bayer4A(vec2(p.y + 1.0, p.x + 3.0));
}

void main() {
    vec2 screenPos = vec2(gl_FragCoord.x, uScreenHeight - gl_FragCoord.y);
    vec2 worldPos = ScreenToWorld(screenPos);
    vec2 worldPx = floor(worldPos);
    float signedDist = distance(worldPos, uZoneCenter) - uZoneRadius;

    const float innerSigmaPx = 2.25;
    const float outerSigmaPx = 12.0;
    float sigma = (signedDist < 0.0) ? innerSigmaPx : outerSigmaPx;
    float density = exp(-0.5 * (signedDist * signedDist) / max(sigma * sigma, 0.0001));
    density *= 0.92;

    float patternSelect = mod(floor(worldPx.x / 16.0) + floor(worldPx.y / 16.0), 2.0);
    float thresholdA = Bayer4A(worldPx);
    float thresholdB = Bayer4B(worldPx);
    float threshold = mix(thresholdA, thresholdB, patternSelect);

    if (density <= threshold) {
        finalColor = vec4(0.0);
        return;
    }

    finalColor = vec4(200.0 / 255.0, 131.0 / 255.0, 200.0 / 255.0, 1.0);
}
