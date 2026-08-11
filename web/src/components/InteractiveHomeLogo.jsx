import { useEffect, useRef, useState } from 'react';
import {
  IDLE_LOGO_LIGHT_RADIUS_PX,
  MAX_LIGHT_DISTANCE_PX,
  clampLightToRadius,
  createRandomLogoLightOffset,
  createDynamicLogoFpsProbe,
  interpolateLogoLightOffset,
} from '../lib/interactiveHomeLogoPerformance.js';

const LOGO_SIZE = 100;
const CANVAS_SIZE = 156;
const MAX_DPR = 1.5;
const FIXED_LIGHT_OFFSET = 30;
const IDLE_LIGHT_DELAY_MS = 2000;
const IDLE_LIGHT_MOVE_DURATION_MS = 1600;
const REDUCED_MOTION_QUERY = '(prefers-reduced-motion: reduce)';
const ICON_CORNER_RADIUS = 0.237;
let lowFpsFallbackLatched = false;

const VERTEX_SHADER = `#version 300 es
layout(location = 0) in vec2 a_position;

void main() {
  gl_Position = vec4(a_position, 0.0, 1.0);
}
`;

const FRAGMENT_SHADER = `#version 300 es
precision highp float;

uniform vec2 u_resolution;
uniform vec2 u_mouse;
uniform vec2 u_center;
uniform float u_size;
uniform float u_theme;
uniform float u_pointer_active;

out vec4 outColor;

float saturate(float value) {
  return clamp(value, 0.0, 1.0);
}

float hash21(vec2 value) {
  return fract(sin(dot(value, vec2(127.1, 311.7))) * 43758.5453123);
}

float sdRoundBox(vec2 point, vec2 bounds, float radius) {
  vec2 delta = abs(point) - bounds + radius;
  return min(max(delta.x, delta.y), 0.0) +
    length(max(delta, 0.0)) - radius;
}

float sdSegment(vec2 point, vec2 start, vec2 end) {
  vec2 pointVector = point - start;
  vec2 segmentVector = end - start;
  float ratio = clamp(
    dot(pointVector, segmentVector) / dot(segmentVector, segmentVector),
    0.0,
    1.0
  );
  return length(pointVector - segmentVector * ratio);
}

vec2 iconPoint(float x, float y) {
  return vec2((x - 256.0) / 472.0, (256.0 - y) / 472.0 + 0.020);
}

vec2 cubicBezier(
  vec2 start,
  vec2 controlOne,
  vec2 controlTwo,
  vec2 end,
  float ratio
) {
  float oneMinus = 1.0 - ratio;
  return oneMinus * oneMinus * oneMinus * start +
    3.0 * oneMinus * oneMinus * ratio * controlOne +
    3.0 * oneMinus * ratio * ratio * controlTwo +
    ratio * ratio * ratio * end;
}

void accumulatePolygonEdge(
  vec2 point,
  vec2 start,
  vec2 end,
  inout float minimumDistance,
  inout float signValue
) {
  minimumDistance = min(minimumDistance, sdSegment(point, start, end));
  bool startAbove = start.y > point.y;
  bool endAbove = end.y > point.y;

  if (startAbove != endAbove) {
    float crossing = start.x +
      (point.y - start.y) * (end.x - start.x) / (end.y - start.y);
    if (point.x < crossing) signValue = -signValue;
  }
}

void accumulatePolygonCubic(
  vec2 point,
  vec2 start,
  vec2 controlOne,
  vec2 controlTwo,
  vec2 end,
  inout float minimumDistance,
  inout float signValue
) {
  vec2 previous = start;

  for (int index = 1; index <= 8; index++) {
    float ratio = float(index) / 8.0;
    vec2 current = cubicBezier(
      start,
      controlOne,
      controlTwo,
      end,
      ratio
    );
    accumulatePolygonEdge(
      point,
      previous,
      current,
      minimumDistance,
      signValue
    );
    previous = current;
  }
}

float sdLetterA(vec2 point) {
  vec2 p0 = iconPoint(73.0, 384.0);
  vec2 p1 = iconPoint(164.5, 139.5);
  vec2 p2 = iconPoint(192.0, 121.0);
  vec2 p3 = iconPoint(210.0, 121.0);
  vec2 p4 = iconPoint(237.5, 139.5);
  vec2 p5 = iconPoint(329.0, 384.0);
  vec2 p6 = iconPoint(265.0, 384.0);
  vec2 p7 = iconPoint(245.7, 326.0);
  vec2 p8 = iconPoint(154.3, 326.0);
  vec2 p9 = iconPoint(135.0, 384.0);
  float outerDistance = 10.0;
  float outerSign = 1.0;

  accumulatePolygonEdge(point, p0, p1, outerDistance, outerSign);
  accumulatePolygonCubic(
    point,
    p1,
    iconPoint(169.1, 127.2),
    iconPoint(178.3, 121.0),
    p2,
    outerDistance,
    outerSign
  );
  accumulatePolygonEdge(point, p2, p3, outerDistance, outerSign);
  accumulatePolygonCubic(
    point,
    p3,
    iconPoint(223.7, 121.0),
    iconPoint(232.9, 127.2),
    p4,
    outerDistance,
    outerSign
  );
  accumulatePolygonEdge(point, p4, p5, outerDistance, outerSign);
  accumulatePolygonEdge(point, p5, p6, outerDistance, outerSign);
  accumulatePolygonEdge(point, p6, p7, outerDistance, outerSign);
  accumulatePolygonEdge(point, p7, p8, outerDistance, outerSign);
  accumulatePolygonEdge(point, p8, p9, outerDistance, outerSign);
  accumulatePolygonEdge(point, p9, p0, outerDistance, outerSign);

  vec2 h0 = iconPoint(174.7, 266.0);
  vec2 h1 = iconPoint(225.3, 266.0);
  vec2 h2 = iconPoint(200.3, 190.8);
  float holeDistance = 10.0;
  float holeSign = 1.0;
  accumulatePolygonEdge(point, h0, h1, holeDistance, holeSign);
  accumulatePolygonEdge(point, h1, h2, holeDistance, holeSign);
  accumulatePolygonEdge(point, h2, h0, holeDistance, holeSign);

  return max(outerDistance * outerSign, -holeDistance * holeSign);
}

float sdPrompt(vec2 point) {
  float halfThickness = 19.0 / 472.0;
  vec2 start = iconPoint(348.0, 203.0);
  vec2 joinPoint = iconPoint(431.0, 256.0);
  vec2 end = iconPoint(348.0, 309.0);
  vec2 upperDirection = normalize(joinPoint - start);
  vec2 lowerDirection = normalize(end - joinPoint);
  vec2 upperNormal = vec2(-upperDirection.y, upperDirection.x);
  vec2 lowerNormal = vec2(-lowerDirection.y, lowerDirection.x);
  vec2 miterDirection = normalize(upperNormal + lowerNormal);
  float miterLength = halfThickness /
    max(dot(miterDirection, upperNormal), 0.001);

  vec2 startCap = start - upperDirection * halfThickness;
  vec2 endCap = end + lowerDirection * halfThickness;
  vec2 outerJoin = joinPoint + miterDirection * miterLength;
  vec2 innerJoin = joinPoint - miterDirection * miterLength;
  vec2 v0 = startCap + upperNormal * halfThickness;
  vec2 v1 = outerJoin;
  vec2 v2 = endCap + lowerNormal * halfThickness;
  vec2 v3 = endCap - lowerNormal * halfThickness;
  vec2 v4 = innerJoin;
  vec2 v5 = startCap - upperNormal * halfThickness;
  float minimumDistance = 10.0;
  float signValue = 1.0;

  accumulatePolygonEdge(point, v0, v1, minimumDistance, signValue);
  accumulatePolygonEdge(point, v1, v2, minimumDistance, signValue);
  accumulatePolygonEdge(point, v2, v3, minimumDistance, signValue);
  accumulatePolygonEdge(point, v3, v4, minimumDistance, signValue);
  accumulatePolygonEdge(point, v4, v5, minimumDistance, signValue);
  accumulatePolygonEdge(point, v5, v0, minimumDistance, signValue);

  return minimumDistance * signValue;
}

float glyphDistance(vec2 point) {
  return min(sdLetterA(point), sdPrompt(point));
}

float tileHeight(vec2 point) {
  float tile = sdRoundBox(point, vec2(0.5), ${ICON_CORNER_RADIUS});
  float bevel = 1.0 - smoothstep(-0.055, 0.008, tile);
  float crown = saturate(
    1.0 - dot(point * vec2(0.80, 0.76), point * vec2(0.80, 0.76))
  );
  return bevel * (0.014 + 0.026 * crown);
}

void main() {
  vec2 fragment = gl_FragCoord.xy;
  vec2 point = (fragment - u_center) / u_size;
  float pixel = max(1.25 / u_size, 0.0012);
  float lightTheme = step(0.5, u_theme);
  float pointerActivity = step(0.5, u_pointer_active);

  if (max(abs(point.x), abs(point.y)) >= 0.78) {
    outColor = vec4(0.0);
    return;
  }

  float tileDistance = sdRoundBox(
    point,
    vec2(0.5),
    ${ICON_CORNER_RADIUS}
  );
  float tileMask = 1.0 - smoothstep(-pixel, pixel, tileDistance);

  vec2 rawLightPlanar = (u_mouse - u_center) / u_size;
  float requestedLightDistance = length(rawLightPlanar);
  float maximumLightDistance = 0.80;
  float rawLightDistance = min(requestedLightDistance, maximumLightDistance);
  vec2 lightPlanar = rawLightPlanar;
  if (requestedLightDistance > maximumLightDistance) {
    lightPlanar *= maximumLightDistance / requestedLightDistance;
  }

  float lightDistanceToTile = max(rawLightDistance - 0.50, 0.0);
  float shadowDistanceFactor = smoothstep(0.0, 0.30, lightDistanceToTile);
  float darkShadowLength = mix(0.024, 0.20, shadowDistanceFactor);
  vec2 darkShadowDirection = normalize(-rawLightPlanar + vec2(0.0001));
  vec2 lightPointerShadowDirection = requestedLightDistance > 0.001
    ? normalize(-rawLightPlanar)
    : vec2(0.0, -1.0);
  float lightShadowLength = mix(
    0.025,
    0.0375,
    pointerActivity * shadowDistanceFactor
  );
  vec2 lightShadowDirection = normalize(mix(
    vec2(0.0, -1.0),
    lightPointerShadowDirection,
    pointerActivity
  ));
  float shadowLength = mix(darkShadowLength, lightShadowLength, lightTheme);
  vec2 shadowDirection = normalize(mix(
    darkShadowDirection,
    lightShadowDirection,
    lightTheme
  ));
  float castShadow = 0.0;

  for (int index = 1; index <= 12; index++) {
    float travel = float(index) / 12.0;
    float shadowDistance = sdRoundBox(
      point - shadowDirection * (travel * shadowLength),
      vec2(0.5),
      ${ICON_CORNER_RADIUS}
    );
    float penumbra = mix(0.018, 0.040, travel);
    float sampleShadow = 1.0 - smoothstep(
      -0.008 - travel * 0.003,
      penumbra,
      shadowDistance
    );
    castShadow = max(castShadow, sampleShadow * (1.0 - travel * 0.38));
  }

  float shadowStrength = mix(0.44, 0.18, lightTheme);
  float shadowAlpha = castShadow * (1.0 - tileMask) * shadowStrength;
  float tileHalo = exp(-max(tileDistance, 0.0) * 12.0) * (1.0 - tileMask);
  float haloAlpha = tileHalo * mix(0.13, 0.035, lightTheme);

  float lightHeight = 0.44;
  float lightIntensity = 0.64;
  float specularPower = 38.0;
  vec3 toLight = vec3(lightPlanar - point, lightHeight);
  float lightDistanceSquared = dot(toLight, toLight);
  vec3 lightDirection = normalize(toLight);
  vec3 viewDirection = vec3(0.0, 0.0, 1.0);

  float tileSurfaceHeight = tileHeight(point);
  vec3 tileNormal = normalize(vec3(
    -dFdx(tileSurfaceHeight) * u_size,
    -dFdy(tileSurfaceHeight) * u_size,
    1.0
  ));

  float attenuation = lightIntensity / (0.62 + lightDistanceSquared * 1.18);
  float diffuse = max(dot(tileNormal, lightDirection), 0.0);
  float daylightDiffuse = max(dot(tileNormal, viewDirection), 0.0);
  vec3 halfway = normalize(lightDirection + viewDirection);
  float specular = pow(max(dot(tileNormal, halfway), 0.0), specularPower);
  float fresnel = pow(
    1.0 - max(dot(tileNormal, viewDirection), 0.0),
    3.0
  );

  float paintedGradient = saturate(
    0.46 + point.x * 0.70 - point.y * 0.76
  );
  float balancedPaintedGradient = mix(
    paintedGradient,
    0.46,
    lightTheme * 0.72
  );
  vec3 brandCyan = vec3(0.220, 0.835, 0.969);
  vec3 brandSky = vec3(0.086, 0.529, 0.855);
  vec3 brandBlue = vec3(0.145, 0.388, 0.922);
  vec3 brandDeep = vec3(0.031, 0.165, 0.322);
  vec3 tileColor = mix(
    brandCyan,
    brandSky,
    smoothstep(0.0, 0.38, balancedPaintedGradient)
  );
  tileColor = mix(
    tileColor,
    brandBlue,
    smoothstep(0.38, 0.70, balancedPaintedGradient)
  );
  tileColor = mix(
    tileColor,
    brandDeep,
    smoothstep(0.70, 1.0, balancedPaintedGradient)
  );

  vec2 highlightOffset =
    (point - vec2(-0.31, 0.36)) * vec2(1.14, 1.30);
  float paintedHighlight = exp(-dot(highlightOffset, highlightOffset) * 4.2);
  tileColor += vec3(0.72, 0.96, 1.0) *
    paintedHighlight * mix(0.23, 0.04, lightTheme);

  float waveBoundary = -0.215 +
    sin((point.x + 0.08) * 6.2) * 0.055 - point.x * 0.025;
  float lowerDepth = 1.0 - smoothstep(
    waveBoundary - 0.025,
    waveBoundary + 0.025,
    point.y
  );
  tileColor *= 1.0 - lowerDepth * mix(0.10, 0.04, lightTheme);

  float darkTileLighting = 0.56 + diffuse * attenuation * 0.92;
  float lightTileLighting = 0.78 + daylightDiffuse * 0.38 +
    pointerActivity * diffuse * attenuation * 0.14;
  tileColor *= mix(darkTileLighting, lightTileLighting, lightTheme);
  float tileSpecularStrength = mix(
    0.58,
    pointerActivity * 0.16,
    lightTheme
  );
  tileColor += vec3(0.34, 0.82, 1.0) *
    specular * attenuation * tileSpecularStrength;
  tileColor += vec3(0.04, 0.28, 0.48) * fresnel;

  float edgeLight = 1.0 - smoothstep(0.0, 0.034, abs(tileDistance));
  float darkTileEdge = 0.055 + diffuse * 0.11;
  float lightTileEdge = 0.040 + daylightDiffuse * 0.035 +
    pointerActivity * diffuse * attenuation * 0.025;
  tileColor += vec3(0.74, 0.94, 1.0) *
    edgeLight * mix(darkTileEdge, lightTileEdge, lightTheme);
  tileColor *= mix(1.0, 0.80, lightTheme);

  vec2 planarDirection = normalize(lightPlanar + vec2(0.0001));
  float letterDistance = sdLetterA(point);
  float promptDistance = sdPrompt(point);
  float glyph = min(letterDistance, promptDistance);
  float letterMask = 1.0 - smoothstep(-pixel, pixel, letterDistance);
  float promptMask = 1.0 - smoothstep(-pixel, pixel, promptDistance);
  float glyphMask = max(letterMask, promptMask);
  float glyphShadowDistance = glyphDistance(
    point + planarDirection * (0.026 + 0.012 / lightHeight)
  );
  float glyphShadow = 1.0 - smoothstep(-0.004, 0.026, glyphShadowDistance);
  glyphShadow *= 1.0 - glyphMask;
  float glyphReliefStrength = mix(1.0, 0.42, lightTheme);
  tileColor *= 1.0 - glyphShadow * 0.32 * glyphReliefStrength;

  float glyphSurfaceHeight = 0.024 *
    (1.0 - smoothstep(-0.012, 0.010, glyph));
  vec3 glyphNormal = normalize(vec3(
    -dFdx(glyphSurfaceHeight) * u_size,
    -dFdy(glyphSurfaceHeight) * u_size,
    1.0
  ));

  float glyphDiffuse = max(dot(glyphNormal, lightDirection), 0.0);
  float glyphDaylightDiffuse = max(dot(glyphNormal, viewDirection), 0.0);
  float glyphSpecular = pow(
    max(dot(glyphNormal, halfway), 0.0),
    specularPower * 0.72
  );
  float letterGradient = saturate(0.48 - point.y * 1.25 + point.x * 0.16);
  float promptGradient = saturate(0.52 - point.y * 1.90 + point.x * 0.20);
  vec3 letterColor = mix(
    vec3(1.0),
    vec3(0.878, 0.949, 0.996),
    letterGradient
  );
  vec3 promptColor = mix(
    vec3(0.812, 0.980, 0.996),
    vec3(0.404, 0.910, 0.976),
    promptGradient
  );
  vec3 glyphColor = mix(letterColor, promptColor, promptMask);
  float darkGlyphLighting = 0.76 + glyphDiffuse * attenuation * 0.48;
  float lightGlyphLighting = 0.90 + glyphDaylightDiffuse * 0.12 +
    pointerActivity * glyphDiffuse * attenuation * 0.08;
  glyphColor *= mix(darkGlyphLighting, lightGlyphLighting, lightTheme);
  float glyphSpecularStrength = mix(
    0.72,
    pointerActivity * 0.18,
    lightTheme
  );
  glyphColor += vec3(0.72, 0.94, 1.0) *
    glyphSpecular * attenuation * glyphSpecularStrength;

  float glyphEdge = 1.0 - smoothstep(0.002, 0.018, abs(glyph));
  glyphColor += vec3(0.20, 0.48, 0.66) *
    glyphEdge * (0.07 + glyphDiffuse * 0.10) * glyphReliefStrength;
  tileColor = mix(tileColor, glyphColor, glyphMask);

  float surfaceGrain = hash21(floor(fragment));
  tileColor += (surfaceGrain - 0.5) * 0.007;
  tileColor = pow(max(tileColor, 0.0), vec3(0.96));

  vec3 shadowColor = mix(
    vec3(0.0, 0.012, 0.032),
    vec3(0.055, 0.070, 0.085),
    lightTheme
  );
  vec3 haloColor = vec3(0.015, 0.175, 0.285);
  vec3 premultiplied = shadowColor * shadowAlpha;
  float alpha = shadowAlpha;
  premultiplied += haloColor * haloAlpha * (1.0 - alpha);
  alpha += haloAlpha * (1.0 - alpha);
  premultiplied = premultiplied * (1.0 - tileMask) + tileColor * tileMask;
  alpha = alpha * (1.0 - tileMask) + tileMask;

  vec3 color = premultiplied / max(alpha, 0.0001);
  outColor = vec4(color, alpha);
}
`;

function compileShader(gl, type, source) {
  const shader = gl.createShader(type);
  if (!shader) throw new Error('Unable to create WebGL shader');

  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const message = gl.getShaderInfoLog(shader) || 'WebGL shader compilation failed';
    gl.deleteShader(shader);
    throw new Error(message);
  }
  return shader;
}

export default function InteractiveHomeLogo({ className = '' }) {
  const canvasRef = useRef(null);
  const [rendererRevision, setRendererRevision] = useState(0);
  const [ready, setReady] = useState(false);
  const [lowFpsFallback, setLowFpsFallback] = useState(lowFpsFallbackLatched);

  useEffect(() => {
    setReady(false);
    if (lowFpsFallback) return undefined;

    const canvas = canvasRef.current;
    if (!canvas) return undefined;

    const gl = canvas.getContext('webgl2', {
      alpha: true,
      antialias: false,
      depth: false,
      powerPreference: 'low-power',
      premultipliedAlpha: false,
      preserveDrawingBuffer: false,
    });
    if (!gl) return undefined;

    let vertexShader = null;
    let fragmentShader = null;
    let program = null;
    let buffer = null;

    try {
      vertexShader = compileShader(gl, gl.VERTEX_SHADER, VERTEX_SHADER);
      fragmentShader = compileShader(gl, gl.FRAGMENT_SHADER, FRAGMENT_SHADER);
      program = gl.createProgram();
      if (!program) throw new Error('Unable to create WebGL program');

      gl.attachShader(program, vertexShader);
      gl.attachShader(program, fragmentShader);
      gl.linkProgram(program);
      if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        throw new Error(gl.getProgramInfoLog(program) || 'WebGL program link failed');
      }

      buffer = gl.createBuffer();
      if (!buffer) throw new Error('Unable to create WebGL buffer');
    } catch (error) {
      console.warn('Interactive ACECode logo initialization failed; using static fallback.', error);
      if (buffer) gl.deleteBuffer(buffer);
      if (program) gl.deleteProgram(program);
      if (fragmentShader) gl.deleteShader(fragmentShader);
      if (vertexShader) gl.deleteShader(vertexShader);
      return undefined;
    }

    gl.useProgram(program);
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      new Float32Array([-1, -1, 3, -1, -1, 3]),
      gl.STATIC_DRAW,
    );
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

    const uniforms = {
      resolution: gl.getUniformLocation(program, 'u_resolution'),
      mouse: gl.getUniformLocation(program, 'u_mouse'),
      center: gl.getUniformLocation(program, 'u_center'),
      size: gl.getUniformLocation(program, 'u_size'),
      theme: gl.getUniformLocation(program, 'u_theme'),
      pointerActive: gl.getUniformLocation(program, 'u_pointer_active'),
    };
    const pointer = { clientX: 0, clientY: 0, active: false };
    const idleLight = {
      active: false,
      currentX: 0,
      currentY: 0,
      startX: 0,
      startY: 0,
      targetX: 0,
      targetY: 0,
      segmentStartedAt: null,
    };
    const reducedMotionQuery = window.matchMedia?.(REDUCED_MOTION_QUERY) || null;
    let reducedMotion = reducedMotionQuery?.matches === true;
    let animationFrame = 0;
    let idleTimer = 0;
    let disposed = false;
    let contextLost = false;
    let revealed = false;
    let lowFpsFallbackTriggered = false;
    const fpsProbe = createDynamicLogoFpsProbe();

    const cancelScheduledFrame = () => {
      if (animationFrame !== 0) window.cancelAnimationFrame(animationFrame);
      animationFrame = 0;
    };
    const clearIdleTimer = () => {
      if (idleTimer !== 0) window.clearTimeout(idleTimer);
      idleTimer = 0;
    };
    const stopIdleMotion = ({ cancelFrame = false } = {}) => {
      clearIdleTimer();
      idleLight.active = false;
      idleLight.segmentStartedAt = null;
      fpsProbe.reset();
      if (cancelFrame) cancelScheduledFrame();
    };
    const chooseNextIdleTarget = () => {
      const target = createRandomLogoLightOffset(
        Math.random,
        IDLE_LOGO_LIGHT_RADIUS_PX,
      );
      idleLight.startX = idleLight.currentX;
      idleLight.startY = idleLight.currentY;
      idleLight.targetX = target.x;
      idleLight.targetY = target.y;
    };
    const advanceIdleLight = (frameTimestamp) => {
      if (!idleLight.active) return;
      if (idleLight.segmentStartedAt === null) {
        idleLight.segmentStartedAt = frameTimestamp;
      }

      const progress = (
        frameTimestamp - idleLight.segmentStartedAt
      ) / IDLE_LIGHT_MOVE_DURATION_MS;
      const position = interpolateLogoLightOffset(
        { x: idleLight.startX, y: idleLight.startY },
        { x: idleLight.targetX, y: idleLight.targetY },
        progress,
      );
      idleLight.currentX = position.x;
      idleLight.currentY = position.y;

      if (progress >= 1) {
        chooseNextIdleTarget();
        idleLight.segmentStartedAt = frameTimestamp;
      }
    };
    const startIdleMotion = () => {
      idleTimer = 0;
      if (
        disposed
        || contextLost
        || lowFpsFallbackTriggered
        || document.hidden
        || reducedMotion
      ) {
        return;
      }

      const rect = canvas.getBoundingClientRect();
      if (rect.width <= 0 || rect.height <= 0) {
        scheduleIdleMotion();
        return;
      }

      const centerX = rect.width * 0.5;
      const centerY = rect.height * 0.5;
      const requestedLightX = pointer.active
        ? pointer.clientX - rect.left
        : centerX - LOGO_SIZE * 0.5 - FIXED_LIGHT_OFFSET;
      const requestedLightY = pointer.active
        ? rect.bottom - pointer.clientY
        : centerY + LOGO_SIZE * 0.5 + FIXED_LIGHT_OFFSET;
      const currentLight = clampLightToRadius(
        requestedLightX,
        requestedLightY,
        centerX,
        centerY,
        MAX_LIGHT_DISTANCE_PX,
      );

      idleLight.active = true;
      idleLight.currentX = currentLight.x - centerX;
      idleLight.currentY = currentLight.y - centerY;
      idleLight.segmentStartedAt = null;
      chooseNextIdleTarget();
      fpsProbe.start({ continuous: true });
      scheduleFrame();
    };
    const scheduleIdleMotion = () => {
      clearIdleTimer();
      if (
        idleLight.active
        || disposed
        || contextLost
        || lowFpsFallbackTriggered
        || document.hidden
        || reducedMotion
      ) {
        return;
      }
      idleTimer = window.setTimeout(startIdleMotion, IDLE_LIGHT_DELAY_MS);
    };

    const draw = (frameTimestamp) => {
      animationFrame = 0;
      if (
        disposed
        || contextLost
        || lowFpsFallbackTriggered
        || document.hidden
      ) {
        return;
      }

      const rect = canvas.getBoundingClientRect();
      if (rect.width <= 0 || rect.height <= 0) return;

      const fpsSample = fpsProbe.sample(frameTimestamp);
      if (fpsSample.belowMinimum) {
        stopIdleMotion();
        lowFpsFallbackTriggered = true;
        lowFpsFallbackLatched = true;
        setReady(false);
        setLowFpsFallback(true);
        console.warn(
          `Interactive ACECode logo measured ${fpsSample.framesPerSecond.toFixed(1)} FPS; using static fallback.`,
        );
        return;
      }

      const pixelRatio = Math.min(window.devicePixelRatio || 1, MAX_DPR);
      const width = Math.max(1, Math.round(rect.width * pixelRatio));
      const height = Math.max(1, Math.round(rect.height * pixelRatio));
      if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
        gl.viewport(0, 0, width, height);
      }

      const centerX = rect.width * 0.5;
      const centerY = rect.height * 0.5;
      advanceIdleLight(frameTimestamp);
      const requestedLightX = idleLight.active
        ? centerX + idleLight.currentX
        : pointer.active
        ? pointer.clientX - rect.left
        : centerX - LOGO_SIZE * 0.5 - FIXED_LIGHT_OFFSET;
      const requestedLightY = idleLight.active
        ? centerY + idleLight.currentY
        : pointer.active
        ? rect.bottom - pointer.clientY
        : centerY + LOGO_SIZE * 0.5 + FIXED_LIGHT_OFFSET;
      const light = clampLightToRadius(
        requestedLightX,
        requestedLightY,
        centerX,
        centerY,
        MAX_LIGHT_DISTANCE_PX,
      );
      const lightTheme = document.documentElement.dataset.theme === 'dark' ? 0 : 1;

      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.uniform2f(uniforms.resolution, width, height);
      gl.uniform2f(uniforms.mouse, light.x * pixelRatio, light.y * pixelRatio);
      gl.uniform2f(uniforms.center, centerX * pixelRatio, centerY * pixelRatio);
      gl.uniform1f(uniforms.size, LOGO_SIZE * pixelRatio);
      gl.uniform1f(uniforms.theme, lightTheme);
      gl.uniform1f(
        uniforms.pointerActive,
        pointer.active || idleLight.active ? 1 : 0,
      );
      gl.drawArrays(gl.TRIANGLES, 0, 3);

      if (!revealed) {
        revealed = true;
        setReady(true);
      }

      if (idleLight.active || fpsSample.shouldContinue) {
        scheduleFrame();
      }
    };

    const scheduleFrame = () => {
      if (
        animationFrame === 0
        && !disposed
        && !contextLost
        && !lowFpsFallbackTriggered
        && !document.hidden
      ) {
        animationFrame = window.requestAnimationFrame(draw);
      }
    };

    const handlePointerMove = (event) => {
      const idleWasActive = idleLight.active;
      stopIdleMotion({ cancelFrame: idleWasActive });
      const samples = event.getCoalescedEvents?.() || [];
      const sample = samples[samples.length - 1] || event;
      pointer.clientX = sample.clientX;
      pointer.clientY = sample.clientY;
      pointer.active = true;
      fpsProbe.start();
      scheduleFrame();
      scheduleIdleMotion();
    };
    const handleVisibilityChange = () => {
      if (document.hidden) {
        stopIdleMotion({ cancelFrame: true });
        return;
      }
      scheduleFrame();
      scheduleIdleMotion();
    };
    const handleContextLost = (event) => {
      event.preventDefault();
      contextLost = true;
      stopIdleMotion({ cancelFrame: true });
      setReady(false);
    };
    const handleContextRestored = () => {
      setRendererRevision((value) => value + 1);
    };
    const handleReducedMotionChange = (event) => {
      reducedMotion = event.matches;
      stopIdleMotion({ cancelFrame: true });
      scheduleFrame();
      scheduleIdleMotion();
    };

    const resizeObserver = typeof ResizeObserver === 'function'
      ? new ResizeObserver(scheduleFrame)
      : null;
    const themeObserver = new MutationObserver(scheduleFrame);
    resizeObserver?.observe(canvas);
    themeObserver.observe(document.documentElement, {
      attributes: true,
      attributeFilter: ['data-theme'],
    });
    window.addEventListener('pointermove', handlePointerMove, { passive: true });
    window.addEventListener('resize', scheduleFrame, { passive: true });
    document.addEventListener('visibilitychange', handleVisibilityChange);
    reducedMotionQuery?.addEventListener?.('change', handleReducedMotionChange);
    canvas.addEventListener('webglcontextlost', handleContextLost);
    canvas.addEventListener('webglcontextrestored', handleContextRestored);
    scheduleFrame();
    scheduleIdleMotion();

    return () => {
      disposed = true;
      stopIdleMotion({ cancelFrame: true });
      resizeObserver?.disconnect();
      themeObserver.disconnect();
      window.removeEventListener('pointermove', handlePointerMove);
      window.removeEventListener('resize', scheduleFrame);
      document.removeEventListener('visibilitychange', handleVisibilityChange);
      reducedMotionQuery?.removeEventListener?.('change', handleReducedMotionChange);
      canvas.removeEventListener('webglcontextlost', handleContextLost);
      canvas.removeEventListener('webglcontextrestored', handleContextRestored);

      if (!gl.isContextLost()) {
        gl.bindBuffer(gl.ARRAY_BUFFER, null);
        gl.useProgram(null);
        gl.deleteBuffer(buffer);
        gl.deleteProgram(program);
        gl.deleteShader(fragmentShader);
        gl.deleteShader(vertexShader);
      }
    };
  }, [lowFpsFallback, rendererRevision]);

  return (
    <div
      className={`ace-home-logo ${className}`.trim()}
      role="img"
      aria-label="ACECode"
      data-dynamic-logo-ready={ready ? 'true' : 'false'}
      data-dynamic-logo-fallback={lowFpsFallback ? 'low-fps' : undefined}
    >
      <img
        src="/acecode-logo.png"
        alt=""
        width={LOGO_SIZE}
        height={LOGO_SIZE}
        className="ace-home-logo-fallback select-none"
        draggable="false"
        aria-hidden="true"
      />
      <canvas
        ref={canvasRef}
        width={CANVAS_SIZE}
        height={CANVAS_SIZE}
        className="ace-home-logo-canvas"
        aria-hidden="true"
      />
    </div>
  );
}
