#version 450

/* Godot's clustered light culler renders light volumes into a framebuffer with NO attachments at
 * all and records the result purely through image atomics from the fragment shader. Both halves of
 * that are unusual: an attachmentless render pass, and fragment-stage atomics on a storage image.
 * Nothing else in this suite exercises either.
 */
layout(set = 0, binding = 0, r32ui) coherent uniform uimage2D cluster;

void
main()
{
   ivec2 position = ivec2(gl_FragCoord.xy);
   imageAtomicAdd(cluster, position, 1u);
}
