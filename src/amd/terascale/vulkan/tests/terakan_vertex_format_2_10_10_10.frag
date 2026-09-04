#version 450
layout(location = 0) flat in vec4 fetched;
layout(location = 0) out vec4 colour;
void main() { colour = fetched; }
