#version 450

// Verifies Vulkan gl_InstanceIndex with a dynamic storage-buffer base.

struct ObjectData {
    vec4 color;
    vec4 padding0;
    vec4 padding1;
    vec4 padding2;
};

layout(std140, set = 0, binding = 0) readonly buffer ObjectBuffer {
    ObjectData objects[];
} object_buffer;

layout(std140, set = 0, binding = 1) uniform TransformBuffer {
    vec4 position;
} transform_buffer;

layout(location = 0) in vec4 packed_position;
layout(location = 4) in vec2 packed_uv;

layout(push_constant) uniform DrawConstants {
    uint object_base;
} draw_constants;

layout(location = 0) out vec4 vertex_color;

void main()
{
    gl_Position = vec4(packed_position.x * 2.0 - 1.0,
                       packed_uv.x * 2.0 - 1.0,
                       transform_buffer.position.zw);
    gl_PointSize = 1.0;
    uint object_index = (draw_constants.object_base + gl_InstanceIndex + gl_VertexIndex) & 3u;
    vertex_color = object_buffer.objects[object_index].color;
}
