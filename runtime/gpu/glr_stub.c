/* No OpenGL on the Wii: the GPU renderer is compiled out.  These stubs keep
   the shared GE/raster recording code linkable; the software rasterizer is
   always selected because g_glr_active stays zero. */
#include "glr.h"

int g_glr_active;

void glr_state(const GlrState *s) { (void)s; }
int glr_has_texture(uint32_t serial) { (void)serial; return 0; }
void glr_texture(uint32_t serial, int w, int h, const uint32_t *pixels) {
    (void)serial; (void)w; (void)h; (void)pixels;
}
GlrVtx *glr_vertices(int prim, int n) { (void)prim; (void)n; return 0; }
void glr_list_end(void) {}
void glr_fb_init(uint32_t fb, int w, int h, const uint32_t *pixels) {
    (void)fb; (void)w; (void)h; (void)pixels;
}
int glr_init(int gles) { (void)gles; return 0; }
void glr_execute(void) {}
int glr_present(uint32_t fb, int x, int y, int w, int h) {
    (void)fb; (void)x; (void)y; (void)w; (void)h;
    return 0;
}
void glr_present_pixels(const uint32_t *px, int w, int h, int x, int y, int vw, int vh) {
    (void)px; (void)w; (void)h; (void)x; (void)y; (void)vw; (void)vh;
}
void glr_set_scale(int scale) { (void)scale; }
int glr_read_fb(uint32_t fb, uint32_t *out, int *scale) {
    (void)fb; (void)out; (void)scale;
    return 0;
}
