#version 450

/* Samples a depth attachment written by an earlier pass in the same frame, the way a shadow map
 * is consumed, and writes it out so the readback can check it. Encodes the depth into all three
 * colour channels so a partially stale read is still visible.
 */
layout(set = 0, binding = 0) uniform sampler2D source_depth;

layout(location = 0) out vec4 out_color;

void
main()
{
   float depth = texelFetch(source_depth, ivec2(gl_FragCoord.xy), 0).r;
   out_color = vec4(depth, depth, depth, 1.0);
}
