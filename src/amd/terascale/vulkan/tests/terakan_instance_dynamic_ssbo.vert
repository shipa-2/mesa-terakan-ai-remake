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

layout(location = 0) out vec4 vertex_color;

void main()
{
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    gl_PointSize = 1.0;
    vertex_color = object_buffer.objects[gl_InstanceIndex].color;
}
