// Do-nothing compute stub: the fallback for a compute shader that is missing or failed to
// compile on this hardware. Dispatches complete without touching any resource; the effect
// that wanted the shader simply contributes nothing this frame.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {}
