#version 300 es
/* dash mode baseline: the scene copied to the window, no filter */
precision mediump float;
uniform sampler2D uTex;
in highp vec2 vP;
out vec4 oc;
void main() { oc = vec4(texture(uTex, vP * 0.5 + 0.5).rgb, 1.0); }
