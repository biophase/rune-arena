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
uniform float uTime;

float Hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float ValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);

    float a = Hash(i + vec2(0.0, 0.0));
    float b = Hash(i + vec2(1.0, 0.0));
    float c = Hash(i + vec2(0.0, 1.0));
    float d = Hash(i + vec2(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float Fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    for (int i = 0; i < 4; ++i) {
        value += amplitude * ValueNoise(p * frequency);
        frequency *= 2.02;
        amplitude *= 0.5;
    }
    return value;
}

float PebbleRippleContribution(vec2 worldPos, vec2 cell, float cycleIndex, float cycleDuration, float cellSize) {
    vec2 cycleSeed = cell + vec2(cycleIndex * 17.0, cycleIndex * 31.0);
    vec2 center =
        (cell + vec2(Hash(cycleSeed + vec2(3.1, 7.2)), Hash(cycleSeed + vec2(11.4, 5.8)))) * cellSize;
    float startOffset = Hash(cycleSeed + vec2(19.7, 23.3)) * cycleDuration * 0.7;
    float age = mod(uTime - startOffset, cycleDuration);
    float maxLife = 2.8 + Hash(cycleSeed + vec2(2.6, 29.1)) * 1.0;
    if (age < 0.0 || age > maxLife) {
        return 0.0;
    }

    float dist = distance(worldPos, center);
    float travelSpeed = 42.0 + Hash(cycleSeed + vec2(13.3, 17.9)) * 18.0;
    float reachedRadius = age * travelSpeed;
    float reached = step(dist, reachedRadius);
    if (reached <= 0.0) {
        return 0.0;
    }

    float rippleFrequency = 0.1;
    float waveTrain = sin((reachedRadius - dist) * rippleFrequency - age * 2.6);
    waveTrain = smoothstep(0.4, 0.9, waveTrain);

    float distanceFade = 1.0 - smoothstep(reachedRadius * 0.15, reachedRadius + 70.0, dist);
    float lifeFade = 1.0 - smoothstep(maxLife * 0.45, maxLife, age);
    float strength = 0.45 + 0.35 * Hash(cycleSeed + vec2(41.0, 43.0));
    return reached * waveTrain * distanceFade * lifeFade * strength;
}

float PebbleRippleMask(vec2 worldPos) {
    const float kCellSize = 220.0;
    const float kCycleDuration = 4.5;

    vec2 cellCoord = floor(worldPos / kCellSize);
    float cycleIndex = floor(uTime / kCycleDuration);
    float ripple = 0.0;

    for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
            vec2 cell = cellCoord + vec2(float(ox), float(oy));
            ripple += PebbleRippleContribution(worldPos, cell, cycleIndex, kCycleDuration, kCellSize);
            ripple += PebbleRippleContribution(worldPos, cell, cycleIndex - 1.0, kCycleDuration, kCellSize);
        }
    }

    return clamp(ripple * 0.7, 0.0, 1.0);
}

void main() {
    vec4 texel = texture(texture0, fragTexCoord) * fragColor * colDiffuse;
    if (texel.a <= 0.001) {
        finalColor = vec4(0.0);
        return;
    }

    vec2 screenPos = vec2(gl_FragCoord.x, uScreenHeight - gl_FragCoord.y);
    vec2 worldPos = ((screenPos - uCameraOffset) / max(uCameraZoom, 0.0001)) + uCameraTarget;

    vec2 baseUv = worldPos * 0.012;
    vec2 warpUvA = baseUv + vec2(uTime * 0.22, -uTime * 0.17);
    vec2 warpUvB = baseUv * 1.7 + vec2(-uTime * 0.31, uTime * 0.26);
    vec2 warp = vec2(Fbm(warpUvA), Fbm(warpUvB)) - vec2(0.5);

    vec2 noiseUvA = baseUv + warp * 1.6 + vec2(uTime * 0.34, -uTime * 0.21);
    vec2 noiseUvB = baseUv * 1.9 - warp.yx * 1.2 + vec2(-uTime * 0.27, uTime * 0.38);
    float cloudA = Fbm(noiseUvA);
    float cloudB = Fbm(noiseUvB);
    float cloud = mix(cloudA, cloudB, 0.45 + 0.25 * sin(uTime * 0.9 + worldPos.x * 0.01));
    float cloudMask = smoothstep(0.5, 0.62, cloud);
    cloudMask = pow(cloudMask, 1.6);

    float rippleMask = PebbleRippleMask(worldPos);
    float opacityMask = rippleMask * mix(0.7, 1.0, cloudMask);


    finalColor = vec4(texel.rgb, texel.a * cloudMask);
}
