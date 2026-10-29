// Krunker v10.0.4 skin shader 11, the halftone "Pop-it" shader (deob-10.0.4.js:196072-196104; TSL twin :92422-92517):
// a two-tone halftone over a lit base colour. Its uniforms (uColor, uLightColor, uShadowColor, uResolution, the
// repetitions, uLightIntensity) are the bundle's defaults overridden by the skin's shaderArgs.uniforms (client/src/skin_fx.c).
// vNormal is the world face normal (dFdx / dFdy: no normal attribute); cameraPosition's direction comes from the view matrix.
out vec4 kn_frag_color;
in vec3 vWorld;
in vec3 vView;

uniform mat4 camera_world_inverse;
uniform vec3 uColor;
uniform vec2 uResolution;
uniform float uShadowRepetitions;
uniform vec3 uShadowColor;
uniform float uLightRepetitions;
uniform vec3 uLightColor;
uniform float uLightIntensity;

vec3 ambientLight(vec3 lightColor, float lightIntensity) {
    return lightColor * lightIntensity;
}

vec3 directionalLight(vec3 lightColor, float lightIntensity, vec3 normal, vec3 lightPosition, vec3 viewDirection, float specularPower) {
    vec3 lightDirection = normalize(lightPosition);
    vec3 lightReflection = reflect(-lightDirection, normal);
    float shading = dot(normal, lightDirection);
    shading = max(0.0, shading);
    float specular = -dot(lightReflection, viewDirection);
    specular = max(0.0, specular);
    specular = pow(specular, specularPower);
    return lightColor * lightIntensity * (shading + specular);
}

vec3 halftone(vec3 color, float repetitions, vec3 direction, float low, float high, vec3 pointColor, vec3 normal) {
    float intensity = dot(normal, direction);
    intensity = smoothstep(low, high, intensity);
    vec2 uv = gl_FragCoord.xy / uResolution.y;
    uv *= repetitions;
    uv = mod(uv, 1.0);
    float point = distance(uv, vec2(0.5));
    point = 1.0 - step(0.5 * intensity, point);
    return mix(color, pointColor, point);
}

void main() {
    // normalize(vPosition - cameraPosition): the view-space direction to the fragment, back to world space
    vec3 viewDirection = normalize(transpose(mat3(camera_world_inverse)) * normalize(vView));
    vec3 normal = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
    vec3 color = uColor;
    vec3 light = vec3(0.0);
    light += directionalLight(vec3(1.0, 1.0, 1.0), 1.0 - uLightIntensity, normal, vec3(1.0, 1.0, 0.0), viewDirection, 1.0);
    light += ambientLight(vec3(1.0, 1.0, 1.0), uLightIntensity);
    color *= light;
    color = halftone(color, uShadowRepetitions, vec3(-1.0, -1.0, 0.0), -0.8, 1.5, uShadowColor, normal);
    color = halftone(color, uLightRepetitions, vec3(1.0, 1.0, 0.0), 0.5, 1.5, uLightColor, normal);
    kn_frag_color = vec4(color, 1.0);
}
