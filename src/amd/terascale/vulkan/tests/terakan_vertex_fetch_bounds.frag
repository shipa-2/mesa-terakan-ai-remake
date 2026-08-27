#version 450
layout(location = 0) flat in uvec4 fetched;
layout(location = 0) out uvec4 colour;
void main() { colour = fetched; }
