#version 450
// fitest: even instances on the left half, odd on the right; the per-instance float passes on.
layout(location = 0) in vec2 pos;
layout(location = 1) in float tag;
layout(location = 0) flat out float vtag;
void main()
{
   float right = float(gl_InstanceIndex & 1);
   gl_Position = vec4(pos.x * 0.5 - 0.5 + right, pos.y, 0.0, 1.0);
   vtag = tag;
}
