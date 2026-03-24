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
uniform vec2 uMapSizeWorld;
uniform vec2 uSignedDistanceRange;
uniform vec4 uTint;
uniform float uPatternPhase;
uniform float uPatternFrame;
uniform sampler2D uToDistanceTexture;
uniform float uBlendT;

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
    int x = int(mod(p.x, 4.0));
    int y = int(mod(p.y, 4.0));
    int idx = x + y * 4;
    float thresholds[16] = float[16](
        0.0 / 16.0, 2.0 / 16.0, 8.0 / 16.0, 10.0 / 16.0,
        12.0 / 16.0, 14.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0,
        3.0 / 16.0, 1.0 / 16.0, 11.0 / 16.0, 9.0 / 16.0,
        15.0 / 16.0, 13.0 / 16.0, 7.0 / 16.0, 5.0 / 16.0
    );
    return thresholds[idx];
}

float HueToRgb(float p, float q, float t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

vec3 RgbToHsl(vec3 c) {
    float maxC = max(max(c.r, c.g), c.b);
    float minC = min(min(c.r, c.g), c.b);
    float h = 0.0;
    float s = 0.0;
    float l = 0.5 * (maxC + minC);

    float d = maxC - minC;
    if (d > 1.0e-10) {
        s = d / (1.0 - abs(2.0 * l - 1.0));
        if (maxC == c.r) {
            h = mod((c.g - c.b) / d, 6.0);
        } else if (maxC == c.g) {
            h = ((c.b - c.r) / d) + 2.0;
        } else {
            h = ((c.r - c.g) / d) + 4.0;
        }
        h /= 6.0;
        if (h < 0.0) {
            h += 1.0;
        }
    }

    return vec3(h, s, l);
}

vec3 HslToRgb(vec3 c) {
    if (c.y <= 1.0e-10) {
        return vec3(c.z);
    }

    float q = c.z < 0.5 ? c.z * (1.0 + c.y) : c.z + c.y - c.z * c.y;
    float p = 2.0 * c.z - q;
    return vec3(
        HueToRgb(p, q, c.x + 1.0 / 3.0),
        HueToRgb(p, q, c.x),
        HueToRgb(p, q, c.x - 1.0 / 3.0)
    );
}

float DecodeSignedDistance(float encoded) {
    return mix(uSignedDistanceRange.x, uSignedDistanceRange.y, clamp(encoded, 0.0, 1.0));
}

float EdgeDensity(float signedDistPx) {
    const float inwardSigmaPx = 6;
    const float outwardSigmaPx = 1.0;
    if (signedDistPx > uSignedDistanceRange.y || signedDistPx < uSignedDistanceRange.x) {
        return 0.0;
    }
    float sigma = signedDistPx >= 0.0 ? inwardSigmaPx : outwardSigmaPx;
    return exp(-0.5 * (signedDistPx * signedDistPx) / max(sigma * sigma, 0.0001));
}

void main() {
    vec2 screenPos = vec2(gl_FragCoord.x, uScreenHeight - gl_FragCoord.y);
    vec2 worldPos = ScreenToWorld(screenPos);
    if (worldPos.x < 0.0 || worldPos.y < 0.0 || worldPos.x >= uMapSizeWorld.x || worldPos.y >= uMapSizeWorld.y) {
        finalColor = vec4(0.0);
        return;
    }

    vec2 uv = worldPos / max(uMapSizeWorld, vec2(0.0001));
    float fromEncoded = texture(texture0, uv).r;
    float toEncoded = texture(uToDistanceTexture, uv).r;
    float signedDistPx = mix(DecodeSignedDistance(fromEncoded), DecodeSignedDistance(toEncoded), clamp(uBlendT, 0.0, 1.0));
    float density = EdgeDensity(signedDistPx) * 0.8;
    const float densityFloor = 0.00000325;
    if (density <= densityFloor) {
        finalColor = vec4(0.0);
        return;
    }

    vec2 worldPx = floor(worldPos) + vec2(0.5);
    float patternMix = mod(floor(worldPx.x / 8.0) + floor(worldPx.y / 8.0) + uPatternPhase + uPatternFrame, 2.0);
    float threshold = mix(Bayer4A(worldPx), Bayer4B(worldPx), patternMix);
    if (density <= threshold) {
        finalColor = vec4(0.0);
        return;
    }

    float normalizedDensity = clamp((density - densityFloor) / max(1.0 - densityFloor, 0.0001), 0.0, 1.0);
    float binIndex = floor(normalizedDensity * 4.0);
    binIndex = clamp(binIndex, 0.0, 3.0);
    float quantizedLightness = mix(0.4, 0.6, binIndex / 3.0);
    float quantizedAlpha = mix(0.05, 0.20, binIndex / 3.0);

    vec3 hsl = RgbToHsl(uTint.rgb);
    hsl.z = quantizedLightness;
    vec3 quantizedRgb = HslToRgb(hsl);

    finalColor = vec4(quantizedRgb, quantizedAlpha) * fragColor * colDiffuse;
}
