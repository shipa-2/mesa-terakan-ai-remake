#version 450

/* SPIR-V requires an explicit gl_PerVertex redeclaration whenever gl_ClipDistance is used. */
out gl_PerVertex {
   vec4 gl_Position;
   float gl_ClipDistance[2];
};

void
main()
{
   /* Classic 3-vertex fullscreen triangle covering [-1, 3] on both axes, well past NDC space. */
   vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
   gl_Position = vec4(position, 0.0, 1.0);
   /* Two distances rather than one, because one was all that ever worked. Positive on the right
    * half of NDC space and positive on the top half, so only the quadrant where both are
    * non-negative survives; the other three keep the clear colour. A driver that honours only the
    * first distance leaves half the target drawn instead of a quarter.
    */
   gl_ClipDistance[0] = position.x;
   gl_ClipDistance[1] = position.y;
}
