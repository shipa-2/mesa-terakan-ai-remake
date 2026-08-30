#version 450

/* A horizontal line across the middle of the target, drawn as a two-vertex line list. */
void
main()
{
   const float x = gl_VertexIndex == 0 ? -1.0 : 1.0;
   gl_Position = vec4(x, 0.0, 0.0, 1.0);
}
