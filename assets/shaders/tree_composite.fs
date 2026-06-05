#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform sampler2D uTrunkTexture;
uniform sampler2D uCanopyForegroundTexture;
uniform sampler2D uMaskTexture;
uniform vec4 colDiffuse;
uniform vec4 uCanopyBackgroundRectPx;
uniform vec4 uTrunkRectPx;
uniform vec4 uCanopyForegroundRectPx;
uniform vec4 uMaskRectPx;
uniform float uTime;
uniform float uSwayStrengthPixels;
uniform float uSwaySpeed;
uniform float uPhaseOffset;
uniform float uGradientStart;
uniform int uRevealCount;
uniform vec4 uRevealData[8];
uniform float uScreenHeight;
uniform float uInsideAlpha;

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

vec4 AlphaOver(vec4 under, vec4 over) {
    float outAlpha = over.a + under.a * (1.0 - over.a);
    if (outAlpha <= 0.0001) {
        return vec4(0.0);
    }
    vec3 outRgb = (over.rgb * over.a + under.rgb * under.a * (1.0 - over.a)) / outAlpha;
    return vec4(outRgb, outAlpha);
}

bool IsVisibleRed(vec4 texel) {
    return texel.a > 0.01 && texel.r > 0.98 && texel.g < 0.02 && texel.b < 0.02;
}

bool IsMaskTexel(vec4 texel) {
    return texel.a > 0.01 && texel.r > 0.45 && texel.r > texel.g + 0.08 && texel.r > texel.b + 0.08;
}

float ComputeWindOffsetPx(vec2 localUv) {
    float clampedY = clamp(localUv.y, 0.0, 1.0);
    float topGradient = 1.0 - smoothstep(uGradientStart, 1.0, clampedY);
    float wave = sin(uTime * uSwaySpeed + uPhaseOffset + clampedY * 5.5);
    return round(wave * uSwayStrengthPixels * topGradient);
}

vec4 SampleLayer(sampler2D atlas, vec4 rectPx, vec2 localPx, bool applyWind) {
    if (rectPx.z <= 0.0 || rectPx.w <= 0.0) {
        return vec4(0.0);
    }

    vec2 frameSize = rectPx.zw;
    vec2 samplePx = localPx;
    if (applyWind) {
        float windOffsetPx = ComputeWindOffsetPx((localPx + vec2(0.5)) / frameSize);
        samplePx.x -= windOffsetPx;
    }

    if (samplePx.x < 0.0 || samplePx.y < 0.0 || samplePx.x >= frameSize.x || samplePx.y >= frameSize.y) {
        return vec4(0.0);
    }

    vec2 atlasSize = vec2(textureSize(atlas, 0));
    vec2 sampleUv = (rectPx.xy + samplePx + vec2(0.5)) / atlasSize;
    return texture(atlas, sampleUv);
}

vec4 ComposeVisible(vec2 localPx) {
    vec4 merged = SampleLayer(texture0, uCanopyBackgroundRectPx, localPx, true);
    merged = AlphaOver(merged, SampleLayer(uTrunkTexture, uTrunkRectPx, localPx, false));
    merged = AlphaOver(merged, SampleLayer(uCanopyForegroundTexture, uCanopyForegroundRectPx, localPx, true));
    return merged;
}

vec4 EvaluateComposite(vec2 localPx, out bool masked) {
    vec4 visible = ComposeVisible(localPx);
    vec4 maskTexel = SampleLayer(uMaskTexture, uMaskRectPx, localPx, true);
    masked = IsVisibleRed(visible) || IsMaskTexel(maskTexel);
    return masked ? vec4(0.0) : visible;
}

float ComputeReveal(vec2 screenPos) {
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
    return reveal;
}

void main() {
    vec2 canopyAtlasSize = vec2(textureSize(texture0, 0));
    vec2 localPx = fragTexCoord * canopyAtlasSize - uCanopyBackgroundRectPx.xy;

    bool masked = false;
    vec4 composed = EvaluateComposite(localPx, masked);
    vec4 color = vec4(0.0);

    if (!masked && composed.a > 0.01) {
        color = composed;
    } else if (!masked) {
        vec2 offsets[4] = vec2[4](vec2(0.0, -1.0), vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0));
        bool outline = false;
        for (int i = 0; i < 4; ++i) {
            bool neighborMasked = false;
            vec4 neighbor = EvaluateComposite(localPx + offsets[i], neighborMasked);
            if (!neighborMasked && neighbor.a > 0.01) {
                outline = true;
                break;
            }
        }
        if (outline) {
            color = vec4(94.0 / 255.0, 80.0 / 255.0, 59.0 / 255.0, 1.0);
        }
    }

    if (color.a > 0.0 && uRevealCount > 0) {
        vec2 screenPos = vec2(gl_FragCoord.x, uScreenHeight - gl_FragCoord.y);
        float reveal = ComputeReveal(screenPos);
        if (reveal > 0.0) {
            float alphaMul = 1.0;
            if (reveal >= 0.999) {
                alphaMul = uInsideAlpha;
            } else {
                float dither = Bayer4(floor(localPx));
                alphaMul = (reveal > dither) ? uInsideAlpha : 1.0;
            }
            color.a *= alphaMul;
        }
    }

    finalColor = color * fragColor * colDiffuse;
}
