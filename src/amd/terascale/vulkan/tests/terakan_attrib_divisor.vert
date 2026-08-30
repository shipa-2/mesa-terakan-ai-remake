#version 450

/* One horizontal line per instance, at the instance's own row, coloured from a per-instance
 * attribute whose divisor decides which element each instance reads.
 */
layout(location = 0) in vec4 in_colour;
layout(location = 0) out vec4 out_colour;

void
main()
{
   const float x = gl_VertexIndex == 0 ? -1.0 : 1.0;
   const float y = (float(gl_InstanceIndex) + 0.5) / 16.0 * 2.0 - 1.0;
   gl_Position = vec4(x, y, 0.0, 1.0);
   out_colour = in_colour;
}
