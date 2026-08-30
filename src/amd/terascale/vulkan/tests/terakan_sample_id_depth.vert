#version 450

/* A full-screen triangle. The depth comes from the fragment shader, so the position's Z is
 * irrelevant here.
 */
void
main()
{
   const vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
   gl_Position = vec4(position, 0.0, 1.0);
}
