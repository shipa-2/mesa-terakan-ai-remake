#version 450

/* A quad covering exactly the left half of the target, so the right half stays whatever the
 * attachment load op left there and the two can be checked independently.
 */
layout(push_constant) uniform Push {
   vec4 color;
   float depth;
} push;

void
main()
{
   const vec2 quad[6] = vec2[](vec2(-1.0, -1.0), vec2(0.0, -1.0), vec2(0.0, 1.0),
                               vec2(-1.0, -1.0), vec2(0.0, 1.0), vec2(-1.0, 1.0));
   gl_Position = vec4(quad[gl_VertexIndex], push.depth, 1.0);
}
