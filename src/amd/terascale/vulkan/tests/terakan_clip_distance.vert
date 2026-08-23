#version 450

/* SPIR-V requires an explicit gl_PerVertex redeclaration whenever gl_ClipDistance is used. */
out gl_PerVertex {
   vec4 gl_Position;
   float gl_ClipDistance[1];
};

void
main()
{
   /* Classic 3-vertex fullscreen triangle covering [-1, 3] on both axes, well past NDC space. */
   vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
   gl_Position = vec4(position, 0.0, 1.0);
   /* Positive on the right half of NDC space (x >= 0), negative on the left half: the left half
    * must be clipped away, leaving the clear color, while the right half is rasterized normally.
    */
   gl_ClipDistance[0] = position.x;
}
