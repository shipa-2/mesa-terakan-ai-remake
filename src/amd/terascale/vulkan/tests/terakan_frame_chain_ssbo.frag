#version 450

/* Samples the previous pass's attachment for RGB, exactly like terakan_frame_chain.frag, and
 * additionally reads a storage buffer the compute pass wrote this same frame into alpha, so a
 * stale SSBO read -- a hazard the plain image-only chain never exercises -- is visible in the
 * readback independently of the image channel.
 */
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) readonly buffer FrameValue {
   uint value;
} frame_value;

layout(location = 0) out vec4 out_color;

void
main()
{
   vec4 sampled = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
   out_color = vec4(sampled.rgb, float(frame_value.value) / 255.0);
}
