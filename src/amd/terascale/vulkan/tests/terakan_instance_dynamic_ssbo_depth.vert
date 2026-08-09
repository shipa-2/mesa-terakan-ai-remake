#version 450

layout(std140, set = 1, binding = 0) uniform TransformBuffer {
    vec4 position;
} transform_buffer;

layout(push_constant) uniform DrawConstants {
    uint object_base;
} draw_constants;

layout(location = 0) in vec4 packed_position;
layout(location = 4) in vec2 packed_uv;

invariant gl_Position;

void main()
{
    bool triangle = (draw_constants.object_base & 0x80000000u) != 0u;
    if (triangle) {
        const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0),
                                          vec2(3.0, -1.0),
                                          vec2(-1.0, 3.0));
        const float depths[3] = float[3](0.1171875, 0.5234375, 0.8984375);
        const float clip_w[3] = float[3](1.0, 2.0, 4.0);
        uint vertex = uint(gl_VertexIndex) % 3u;
        gl_Position = vec4(positions[vertex] * clip_w[vertex],
                           depths[vertex] * clip_w[vertex], clip_w[vertex]);
    } else {
        gl_Position = vec4(packed_position.x * 2.0 - 1.0,
                           packed_uv.x * 2.0 - 1.0,
                           transform_buffer.position.zw);
    }
    gl_PointSize = 1.0;
}
