#version 450

/* A full-target quad. Each draw is confined to one sample by the pipeline's sample mask, so the
 * colour reaching a given sample is chosen per draw by the fragment shader's push constant.
 */
void
main()
{
   const vec2 quad[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                               vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
   gl_Position = vec4(quad[gl_VertexIndex], 0.0, 1.0);
}
