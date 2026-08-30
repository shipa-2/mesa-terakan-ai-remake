#version 450

/* Gives every sample its own depth, without the pipeline requesting sample shading. Section
 * "Sample Shading" of the Vulkan 1.4.349 specification enables per-sample invocation for a
 * fragment shader whose interface includes the SampleId built-in regardless of
 * sampleShadingEnable, so all four depths must reach the image. If the shader runs once per
 * fragment instead, every sample receives sample 0's depth.
 */
void
main()
{
   gl_FragDepth = 0.1 + 0.2 * float(gl_SampleID);
}
