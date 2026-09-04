#version 450

/* The descriptor shape of a failing dEQP-VK.binding_model.descriptorset_random case, transcribed
 * from the GLSL that test logs, so that the shader body below can be cut down statement by
 * statement while the bindings stay exactly as they were. The case is
 * sets4.dynindexed.ubolimitlow.sbolimitlow.sampledimglow.lowimgnotex.noiub.nouab.frag.noia.8,
 * which loses ten of its sixty-four texels: 44, 50-53 and 58-62.
 *
 * Every value is the one the original expects, and `accum` stays zero for as long as they all
 * match, which is what makes each array index dynamic without being anything but zero.
 */

layout(r32i, set = 0, binding = 0) uniform iimage2D simage0_0;
layout(set = 0, binding = 1) uniform ubodef0_1 { int val; } ubo0_1[9];
layout(set = 0, binding = 3) uniform ubodef0_3 { int val; } ubo0_3;
layout(set = 1, binding = 0) uniform ubodef1_0 { int val; } ubo1_0;
layout(r32i, set = 1, binding = 10) uniform iimage2D simage1_10;
layout(set = 1, binding = 11) uniform itextureBuffer texbo1_11[1];
layout(set = 2, binding = 1) uniform itextureBuffer texbo2_1[9];
layout(set = 2, binding = 2) buffer sbodef2_2 { int val; } ssbo2_2;
layout(set = 2, binding = 3) uniform ubodef2_3 { int val; } ubo2_3[1];
layout(r32i, set = 2, binding = 4) uniform iimage2D simage2_4[2];
layout(set = 2, binding = 10) uniform itextureBuffer texbo2_10[6];
layout(set = 3, binding = 0) buffer sbodef3_0 { int val; } ssbo3_0[1];
layout(set = 3, binding = 1) buffer sbodef3_1 { int val; } ssbo3_1[2];

void
main()
{
  const int invocationID = int(gl_FragCoord.y) * 8 + int(gl_FragCoord.x);
  int accum = 0, temp;
  temp = ubo0_1[accum + 0].val;
  accum |= temp - 1;
  temp = ubo0_1[accum + 1].val;
  accum |= temp - 2;
  temp = ubo0_1[accum + 2].val;
  accum |= temp - 3;
  temp = ubo0_1[accum + 8].val;
  accum |= temp - 9;
  temp = ubo0_3.val;
  accum |= temp - 10;
  temp = ubo1_0.val;
  accum |= temp - 11;
  if (12 == invocationID) imageStore(simage1_10, ivec2(0, 0), ivec4(12, 0, 0, 0));
  temp = texelFetch(texbo1_11[accum + 0], 0).x;
  accum |= temp - 13;
  temp = texelFetch(texbo2_1[accum + 0], 0).x;
  accum |= temp - 14;
  temp = texelFetch(texbo2_1[accum + 1], 0).x;
  accum |= temp - 15;
  temp = texelFetch(texbo2_1[accum + 2], 0).x;
  accum |= temp - 16;
  temp = texelFetch(texbo2_1[accum + 8], 0).x;
  accum |= temp - 22;
  if (23 == invocationID) ssbo2_2.val = 23;
  temp = ubo2_3[accum + 0].val;
  accum |= temp - 24;
  if (25 == invocationID) imageStore(simage2_4[accum + 0], ivec2(0, 0), ivec4(25, 0, 0, 0));
  if (26 == invocationID) imageStore(simage2_4[accum + 1], ivec2(0, 0), ivec4(26, 0, 0, 0));
  temp = texelFetch(texbo2_10[accum + 0], 0).x;
  accum |= temp - 27;
  temp = texelFetch(texbo2_10[accum + 1], 0).x;
  accum |= temp - 28;
  temp = texelFetch(texbo2_10[accum + 2], 0).x;
  accum |= temp - 29;
  temp = texelFetch(texbo2_10[accum + 5], 0).x;
  accum |= temp - 32;
  if (33 == invocationID) ssbo3_0[accum + 0].val = 33;
  temp = ssbo3_1[accum + 0].val;
  accum |= temp - 34;
  temp = ssbo3_1[accum + 1].val;
  accum |= temp - 35;
  imageStore(simage0_0, ivec2(gl_FragCoord.x, gl_FragCoord.y),
             (accum != 0) ? ivec4(0, 0, 0, 0) : ivec4(1, 0, 0, 1));
}
