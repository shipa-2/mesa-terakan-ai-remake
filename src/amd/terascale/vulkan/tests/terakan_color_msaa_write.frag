#version 450

/* Writes one flat colour, so a draw restricted to a single sample gives that sample a value no
 * other sample has.
 */
layout(push_constant) uniform Push {
   vec4 colour;
} push;

layout(location = 0) out vec4 out_colour;

void
main()
{
   out_colour = push.colour;
}
