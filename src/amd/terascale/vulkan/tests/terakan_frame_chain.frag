#version 450

/* Samples the previous pass's attachment and writes it straight through, so any stale texel the
 * texture cache still holds from an earlier frame shows up in the readback.
 */
layout(set = 0, binding = 0) uniform sampler2D source;

layout(location = 0) out vec4 out_color;

void
main()
{
   out_color = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
}
