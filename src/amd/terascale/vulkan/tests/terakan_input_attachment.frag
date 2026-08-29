#version 450

/* subpassLoad carries no coordinate: it reads the input attachment at the fragment's own position.
 * A driver that does not supply that position reads one texel for every fragment.
 */
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput source;
layout(location = 0) out vec4 result;

void
main()
{
   result = subpassLoad(source);
}
