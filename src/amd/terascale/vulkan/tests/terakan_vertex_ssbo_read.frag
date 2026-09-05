#version 450

layout(set = 0, binding = 0, std430) buffer Source { uint values[]; } source;
layout(constant_id = 0) const uint element_count = 1024u;
layout(constant_id = 1) const uint expected_value = 0u;
layout(location = 0) flat in uint vertex_first_wrong;
layout(location = 0) out uvec4 out_color;

void
main()
{
   uint first_wrong = 0xFFFFFFFFu;
   for (uint index = 0u; index < element_count; ++index) {
      if (source.values[index] != expected_value) {
         first_wrong = index;
         break;
      }
   }
   /* x: the first element the vertex stage found wrong, y: the same from this stage. */
   out_color = uvec4(vertex_first_wrong, first_wrong, source.values[0], 1u);
}
