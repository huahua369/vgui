// V09 MSAA depth preview helper.
//
// SDL_GPU cannot resolve depth attachments, so MSAA views re-render opaque
// geometry into a single-sample depth target.  That pass has no color target;
// this fragment shader deliberately writes no output so the pipeline does not
// declare an unused color attachment.

[shader("fragment")]
void main()
{
}
