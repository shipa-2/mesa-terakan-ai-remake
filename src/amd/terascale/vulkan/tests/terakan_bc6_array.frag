#version 450

layout(set = 0, binding = 0) uniform sampler2DArray source_texture;

layout(push_constant) uniform Params {
   vec4 direction_lod;
} params;

layout(location = 0) out vec4 color;

void
main()
{
   color = textureLod(source_texture,
                      vec3(0.5, 0.5, params.direction_lod.z),
                      params.direction_lod.w);
}
