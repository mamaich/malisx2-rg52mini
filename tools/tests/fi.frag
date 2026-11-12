#version 450
layout(location = 0) flat in float vtag;
layout(location = 0) out vec4 color;
void main()
{
   color = vec4(vtag / 255.0, 0.0, 0.0, 1.0);
}
