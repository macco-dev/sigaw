#version 450

layout(push_constant) uniform OverlayPush {
    vec2 surface_size;
    vec4 panel_rect;
} push_data;

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 rel = vec2(
        (gl_VertexIndex == 1 || gl_VertexIndex == 3) ? 1.0 : 0.0,
        (gl_VertexIndex >= 2) ? 1.0 : 0.0
    );
    vec2 panel_pos = push_data.panel_rect.xy + rel * push_data.panel_rect.zw;
    vec2 ndc = vec2(
        panel_pos.x / push_data.surface_size.x * 2.0 - 1.0,
        1.0 - panel_pos.y / push_data.surface_size.y * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = rel;
}
