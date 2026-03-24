#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform float uScreenHeight;
uniform vec2 uCameraTarget;
uniform vec2 uCameraOffset;
uniform float uCameraZoom;
uniform vec2 uFadeRectMin;
uniform vec2 uFadeRectMax;

vec2 ScreenToWorld(vec2 screenPos) {
    return (screenPos - uCameraOffset) / max(uCameraZoom, 0.0001) + uCameraTarget;
}

float Hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec2 screenPos = vec2(gl_FragCoord.x, uScreenHeight - gl_FragCoord.y);
    vec2 worldPos = ScreenToWorld(screenPos);
    vec2 worldPx = floor(worldPos) + vec2(0.5);

    float dx = 0.0;
    if (worldPx.x < uFadeRectMin.x) {
        dx = uFadeRectMin.x - worldPx.x;
    } else if (worldPx.x > uFadeRectMax.x) {
        dx = worldPx.x - uFadeRectMax.x;
    }

    float dy = 0.0;
    if (worldPx.y < uFadeRectMin.y) {
        dy = uFadeRectMin.y - worldPx.y;
    } else if (worldPx.y > uFadeRectMax.y) {
        dy = worldPx.y - uFadeRectMax.y;
    }

    float signedDistPx = 0.0;
    if (dx > 0.0 || dy > 0.0) {
        signedDistPx = length(vec2(dx, dy));
    } else {
        float insideLeft = worldPx.x - uFadeRectMin.x;
        float insideRight = uFadeRectMax.x - worldPx.x;
        float insideTop = worldPx.y - uFadeRectMin.y;
        float insideBottom = uFadeRectMax.y - worldPx.y;
        signedDistPx = -min(min(insideLeft, insideRight), min(insideTop, insideBottom));
    }

    const float fadeMidPx = -24.0;
    const float fadeSlope = 0.5;
    const float fadeInteriorCutoffPx = -56.0;
    if (signedDistPx <= fadeInteriorCutoffPx) {
        finalColor = vec4(0.0);
        return;
    }
    float alpha = 1.0 / (1.0 + exp(-(signedDistPx - fadeMidPx) * fadeSlope));
    float threshold = Hash12(worldPx);
    float scaledAlpha = clamp(alpha, 0.0, 1.0) * 3.0;
    float lowerLevel = floor(scaledAlpha);
    float upperLevel = min(lowerLevel + 1.0, 3.0);
    float levelBlend = scaledAlpha - lowerLevel;
    float quantizedAlpha = mix(lowerLevel, upperLevel, threshold < levelBlend ? 1.0 : 0.0) / 3.0;

    if (quantizedAlpha <= 0.001) {
        finalColor = vec4(0.0);
        return;
    }

    finalColor = vec4(0.0, 0.0, 0.0, quantizedAlpha);
}
