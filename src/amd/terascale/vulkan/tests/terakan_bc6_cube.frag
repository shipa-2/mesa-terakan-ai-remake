#version 450

layout(set = 0, binding = 0) uniform samplerCube source_texture;

layout(push_constant) uniform Params {
   vec4 direction_lod;
} params;

layout(location = 0) out vec4 color;

void
main()
{
   color = textureLod(source_texture, params.direction_lod.xyz, params.direction_lod.w);
}
