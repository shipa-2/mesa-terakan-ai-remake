#version 450

/* A full-target quad at the depth the push constant names. Each draw is restricted to one sample
 * by the pipeline's sample mask, so the depth reaching a given sample is chosen per draw.
 */
layout(push_constant) uniform Push {
   float depth;
} push;

void
main()
{
   const vec2 quad[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                               vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
   gl_Position = vec4(quad[gl_VertexIndex], push.depth, 1.0);
}
