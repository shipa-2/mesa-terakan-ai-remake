#version 450
layout(location = 0) in vec4 attribute_value;
layout(location = 0) flat out vec4 fetched;
void main() {
   vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
   fetched = attribute_value;
}
