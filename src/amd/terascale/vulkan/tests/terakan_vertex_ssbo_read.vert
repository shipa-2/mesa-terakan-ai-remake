#version 450

/* Reads the storage buffer another agent wrote earlier in the same command buffer. Deliberately
 * not `readonly`, because that is what makes the driver take the UAV read path, and it is how
 * dEQP's failing cases declare the buffer.
 *
 * The whole buffer is checked rather than one word: dEQP's cases verify every element, and a
 * defect that only affects some offsets would be invisible in a single read. The reported value is
 * the index of the first element that did not hold what the writer put there, or 0xFFFFFFFF when
 * every one of them did.
 */

layout(set = 0, binding = 0, std430) buffer Source { uint values[]; } source;
layout(constant_id = 0) const uint element_count = 1024u;
layout(constant_id = 1) const uint expected_value = 0u;
layout(location = 0) flat out uint vertex_first_wrong;

void
main()
{
   const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
   gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);

   uint first_wrong = 0xFFFFFFFFu;
   for (uint index = 0u; index < element_count; ++index) {
      if (source.values[index] != expected_value) {
         first_wrong = index;
         break;
      }
   }
   vertex_first_wrong = first_wrong;
}
