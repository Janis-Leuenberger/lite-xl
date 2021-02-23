#include <fmt/core.h>

#include "font_renderer.h"

#include "agg_lcd_distribution_lut.h"
#include "agg_pixfmt_rgb.h"
#include "agg_pixfmt_rgba.h"

#include "font_renderer_alpha.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Important: when a subpixel scale is used the width below will be the width in logical pixel.
// As each logical pixel contains 3 subpixels it means that the 'pixels' pointer
// will hold enough space for '3 * width' uint8_t values.
struct FR_Bitmap {
    agg::int8u *pixels;
    int width, height;
};

static FR_Bitmap *debug_bitmap_to_image_rgb(FR_Bitmap *alpha_bitmap, const int subpixel_scale) {
    const int w = alpha_bitmap->width, h = alpha_bitmap->height;
    const int rgb_comp = 3;

    fmt::print("W: {} H: {}\n", w, h);

    FR_Bitmap *rgb_image = (FR_Bitmap *) malloc(sizeof(FR_Bitmap) + w * h * rgb_comp);
    if (!rgb_image) { return nullptr; }
    rgb_image->pixels = (agg::int8u *) (rgb_image + 1);
    rgb_image->width = w;
    rgb_image->height = h;

    agg::int8u *dst_ptr = rgb_image->pixels, *src_ptr = alpha_bitmap->pixels;
    for (int y = 0; y < alpha_bitmap->height; y++) {
        for (int x = 0; x < alpha_bitmap->width; x++) {
            if (subpixel_scale == 3) {
                dst_ptr[0] = 0xff - src_ptr[0];
                dst_ptr[1] = 0xff - src_ptr[1];
                dst_ptr[2] = 0xff - src_ptr[2];
            } else {
                dst_ptr[0] = 0xff - src_ptr[0];
                dst_ptr[1] = 0xff - src_ptr[0];
                dst_ptr[2] = 0xff - src_ptr[0];
            }
            src_ptr += subpixel_scale;
            dst_ptr += rgb_comp;
        }
    }
    return rgb_image;
}

class FR_Renderer {
public:
    // Conventional LUT values: (1./3., 2./9., 1./9.)
    // The values below are fine tuned as in the Elementary Plot library.

    FR_Renderer(bool hinting, bool kerning, bool subpixel, bool prescale_x) :
        m_renderer(hinting, kerning, subpixel, prescale_x),
        m_lcd_lut(0.448, 0.184, 0.092),
        m_subpixel(subpixel)
    { }

    font_renderer_alpha& renderer_alpha() { return m_renderer; }
    agg::lcd_distribution_lut& lcd_distribution_lut() { return m_lcd_lut; }
    int subpixel_scale() const { return (m_subpixel ? 3 : 1); }

    std::string debug_font_name;

private:
    font_renderer_alpha m_renderer;
    agg::lcd_distribution_lut m_lcd_lut;
    int m_subpixel;
};

FR_Renderer *FR_Renderer_New(unsigned int flags) {
    bool hinting    = ((flags & FR_HINTING)    != 0);
    bool kerning    = ((flags & FR_KERNING)    != 0);
    bool subpixel   = ((flags & FR_SUBPIXEL)   != 0);
    bool prescale_x = ((flags & FR_PRESCALE_X) != 0);
    return new FR_Renderer(hinting, kerning, subpixel, prescale_x);
}

FR_Bitmap* FR_Bitmap_New(FR_Renderer *font_renderer, int width, int height) {
    const int subpixel_scale = font_renderer->subpixel_scale();
    FR_Bitmap *image = (FR_Bitmap *) malloc(sizeof(FR_Bitmap) + width * height * subpixel_scale);
    if (!image) { return NULL; }
    image->pixels = (agg::int8u *) (image + 1);
    image->width = width;
    image->height = height;
    return image;
}

void FR_Bitmap_Free(FR_Bitmap *image) {
  free(image);
}

void FR_Renderer_Free(FR_Renderer *font_renderer) {
    delete font_renderer;
}

int FR_Load_Font(FR_Renderer *font_renderer, const char *filename) {
    bool success = font_renderer->renderer_alpha().load_font(filename);
    if (success) {
        std::string fullname = filename;
        size_t a = fullname.find_last_of("/");
        size_t b = fullname.find_last_of(".");
        font_renderer->debug_font_name = fullname.substr(a + 1, b - a - 1);
    }
    return (success ? 0 : 1);
}

int FR_Get_Font_Height(FR_Renderer *font_renderer, float size) {
    font_renderer_alpha& renderer_alpha = font_renderer->renderer_alpha();
    double ascender, descender;
    renderer_alpha.get_font_vmetrics(ascender, descender);
    int face_height = renderer_alpha.get_face_height();
    float scale = renderer_alpha.scale_for_em_to_pixels(size);
    return int((ascender - descender) * face_height * scale + 0.5);
}

static void glyph_trim_rect(agg::rendering_buffer& ren_buf, FR_Bitmap_Glyph_Metrics& gli, int subpixel_scale) {
    const int height = ren_buf.height();
    int x0 = gli.x0 * subpixel_scale, x1 = gli.x1 * subpixel_scale;
    int y0 = gli.y0, y1 = gli.y1;
    for (int y = gli.y0; y < gli.y1; y++) {
        const uint8_t *row = ren_buf.row_ptr(height - 1 - y);
        unsigned int row_bitsum = 0;
        for (int x = x0; x < x1; x++) {
            row_bitsum |= row[x];
        }
        if (row_bitsum == 0) {
            y0++;
        } else {
            break;
        }
    }
    for (int y = gli.y1 - 1; y >= y0; y--) {
        const uint8_t *row = ren_buf.row_ptr(height - 1 - y);
        unsigned int row_bitsum = 0;
        for (int x = x0; x < x1; x++) {
            row_bitsum |= row[x];
        }
        if (row_bitsum == 0) {
            y1--;
        } else {
            break;
        }
    }
    for (int x = gli.x0 * subpixel_scale; x < gli.x1 * subpixel_scale; x += subpixel_scale) {
        unsigned int xaccu = 0;
        for (int y = y0; y < y1; y++) {
            const uint8_t *row = ren_buf.row_ptr(height - 1 - y);
            for (int i = 0; i < subpixel_scale; i++) {
                xaccu |= row[x + i];
            }
        }
        if (xaccu == 0) {
            x0 += subpixel_scale;
        } else {
            break;
        }
    }
    for (int x = (gli.x1 - 1) * subpixel_scale; x >= x0; x -= subpixel_scale) {
        unsigned int xaccu = 0;
        for (int y = y0; y < y1; y++) {
            const uint8_t *row = ren_buf.row_ptr(height - 1 - y);
            for (int i = 0; i < subpixel_scale; i++) {
                xaccu |= row[x + i];
            }
        }
        if (xaccu == 0) {
            x1 -= subpixel_scale;
        } else {
            break;
        }
    }
    gli.xoff += (x0 / subpixel_scale) - gli.x0;
    gli.yoff += (y0 - gli.y0);
    gli.x0 = x0 / subpixel_scale;
    gli.y0 = y0;
    gli.x1 = x1 / subpixel_scale;
    gli.y1 = y1;
}

static void glyph_lut_convolution(agg::rendering_buffer ren_buf, agg::lcd_distribution_lut& lcd_lut, agg::int8u *covers_buf, FR_Bitmap_Glyph_Metrics& gli) {
    const int subpixel = 3;
    const int x0 = gli.x0, y0 = gli.y0, x1 = gli.x1, y1 = gli.y1;
    const int len = (x1 - x0) * subpixel;
    const int height = ren_buf.height();
    for (int y = y0; y < y1; y++) {
        agg::int8u *covers = ren_buf.row_ptr(height - 1 - y) + x0 * subpixel;
        memcpy(covers_buf, covers, len);
        for (int x = x0 - 1; x < x1 + 1; x++) {
            for (int i = 0; i < subpixel; i++) {
                const int cx = (x - x0) * subpixel + i;
                covers[cx] = lcd_lut.convolution(covers_buf, cx, 0, len - 1);
            }
        }
    }
    gli.x0 -= 1;
    gli.x1 += 1;
    gli.xoff -= 1;
}

static int ceil_to_multiple(int n, int p) {
    return p * ((n + p - 1) / p);
}

static void debug_rgb_image_write_glyphs_bbox(FR_Bitmap *rgb_image,
    int subpixel_scale, int num_chars,
    FR_Bitmap_Glyph_Metrics *glyphs, agg::int32u color)
{
    const int rgb_comp = 3;
    for (int i = 0; i < num_chars; i++) {
        FR_Bitmap_Glyph_Metrics& gi = glyphs[i];

        int y = gi.y0;
        agg::int8u *row = rgb_image->pixels + (rgb_image->width * y + gi.x0) * rgb_comp;
        for (int x = gi.x0; x < gi.x1; x++) {
            row[0] = (color >> 0 ) & 0xff; // (agg::int32u) row[0] * (((color >> 0 ) & 0xff) + 1) / 256;
            row[1] = (color >> 8 ) & 0xff; // (agg::int32u) row[1] * (((color >> 8 ) & 0xff) + 1) / 256;
            row[2] = (color >> 16) & 0xff; // (agg::int32u) row[2] * (((color >> 16) & 0xff) + 1) / 256;
            row += rgb_comp;
        }

        y = gi.y1;
        row = rgb_image->pixels + (rgb_image->width * y + gi.x0) * rgb_comp;
        for (int x = gi.x0; x < gi.x1; x++) {
            row[0] = (color >> 0 ) & 0xff; // (agg::int32u) row[0] * (((color >> 0 ) & 0xff) + 1) / 256;
            row[1] = (color >> 8 ) & 0xff; // (agg::int32u) row[1] * (((color >> 8 ) & 0xff) + 1) / 256;
            row[2] = (color >> 16) & 0xff; // (agg::int32u) row[2] * (((color >> 16) & 0xff) + 1) / 256;
            row += rgb_comp;
        }
    }
}

FR_Bitmap *FR_Bake_Font_Bitmap(FR_Renderer *font_renderer, int font_height,
    int first_char, int num_chars, FR_Bitmap_Glyph_Metrics *glyphs)
{
    font_renderer_alpha& renderer_alpha = font_renderer->renderer_alpha();
    agg::lcd_distribution_lut& lcd_lut = font_renderer->lcd_distribution_lut();
    const int subpixel_scale = font_renderer->subpixel_scale();

    double ascender, descender;
    renderer_alpha.get_font_vmetrics(ascender, descender);
    const int ascender_px  = int(ascender * font_height);
    const int pad_y = 1;

    // When using subpixel font rendering it is needed to leave a padding pixel on the left and on the right.
    // Since each pixel is composed by n subpixel we set below x_start to subpixel_scale instead than zero.
    // Note about the coordinates: they are AGG-like so x is positive toward the right and
    // y is positive in the upper direction.
    const int x_start = subpixel_scale;
    const agg::alpha8 text_color(0xff);
#ifdef FONT_RENDERER_HEIGHT_HACK
    const int font_height_reduced = (font_height * 86) / 100;
#else
    const int font_height_reduced = font_height;
#endif
    renderer_alpha.set_font_height(font_height_reduced);

    int *index = new int[num_chars];
    agg::rect_i *bounds = new agg::rect_i[num_chars];
    int x_size_sum = 0, glyph_count = 0;
    for (int i = 0; i < num_chars; i++) {
        int codepoint = first_char + i;
        index[i] = i;
        if (renderer_alpha.codepoint_bounds(codepoint, subpixel_scale, bounds[i])) {
            // Invalid glyph
            bounds[i].x1 = 0;
            bounds[i].y1 = 0;
            bounds[i].x2 = -1;
            bounds[i].y2 = -1;
        } else {
            if (bounds[i].x2 > bounds[i].x1) {
                x_size_sum += bounds[i].x2 - bounds[i].x1;
                glyph_count++;
            }
        }
    }

    // Simple insertion sort algorithm: https://en.wikipedia.org/wiki/Insertion_sort
    int i = 1;
    while (i < num_chars) {
        int j = i;
        while (j > 0 && bounds[index[j-1]].y2 - bounds[index[j-1]].y1 > bounds[index[j]].y2 - bounds[index[j]].y1) {
            int tmp = index[j];
            index[j] = index[j-1];
            index[j-1] = tmp;
            j = j - 1;
        }
        i = i + 1;
    }

    if (glyph_count == 0) return nullptr;
    const int pixels_width = (x_size_sum / glyph_count) * 16;

    // dry run simulating pixel position to estimate required image's height
    int x = x_start, y = 0, y_bottom = y;
    for (int i = 0; i < num_chars; i++) {
        const agg::rect_i& gbounds = bounds[index[i]];
        if (gbounds.x2 < gbounds.x1) continue;
        if (x + gbounds.x2 + 1 >= pixels_width * subpixel_scale) {
            x = x_start;
            y = y_bottom;
        }
        const int glyph_y_bottom = y - 2 * pad_y - (gbounds.y2 - gbounds.y1);
        y_bottom = (y_bottom > glyph_y_bottom ? glyph_y_bottom : y_bottom);
        x = x + gbounds.x2 + 2 * subpixel_scale;
    }

    const int pixels_height = -y_bottom + 1;
    const int pixel_size = 1;
    fmt::print("image size: {} {}\n", pixels_width, pixels_height);
    FR_Bitmap *image = FR_Bitmap_New(font_renderer, pixels_width, pixels_height);

    agg::int8u *pixels = image->pixels;
    memset(pixels, 0x00, pixels_width * pixels_height * subpixel_scale * pixel_size);
    agg::rendering_buffer ren_buf(pixels, pixels_width * subpixel_scale, pixels_height, -pixels_width * subpixel_scale * pixel_size);

    agg::int8u *cover_swap_buffer = new agg::int8u[pixels_width * subpixel_scale];
    // The variable y_bottom will be used to go down to the next row by taking into
    // account the space occupied by each glyph of the current row along the y direction.
    x = x_start;
    // Set y to the image's height minus one to begin writing glyphs in the upper part of the image.
    y = pixels_height - 1;
    y_bottom = y;
    for (int i = 0; i < num_chars; i++) {
        int codepoint = first_char + index[i];
        const agg::rect_i& gbounds = bounds[index[i]];
        if (gbounds.x2 < gbounds.x1) continue;

        if (x + gbounds.x2 + 1 >= pixels_width * subpixel_scale) {
            // No more space along x, begin writing the row below.
            x = x_start;
            y = y_bottom;
        }

        const int y_baseline = y - pad_y - gbounds.y2;
        const int glyph_y_bottom = y - 2 * pad_y - (gbounds.y2 - gbounds.y1);
        y_bottom = (y_bottom > glyph_y_bottom ? glyph_y_bottom : y_bottom);

        double x_next = x, y_next = y_baseline;
        renderer_alpha.render_codepoint(ren_buf, text_color, x_next, y_next, codepoint, subpixel_scale);
        int x_next_i = (subpixel_scale == 1 ? int(x_next + 1.0) : ceil_to_multiple(x_next + 0.5, subpixel_scale));

        // Below x and x_next_i will always be integer multiples of subpixel_scale.
        // The y coordinate for the glyph below is positive in the bottom direction,
        // like is used by Lite's drawing system.
        FR_Bitmap_Glyph_Metrics& glyph_info = glyphs[index[i]];
        glyph_info.x0 = x / subpixel_scale;
        glyph_info.y0 = pixels_height - 1 - (y_baseline + gbounds.y2 + pad_y);
        glyph_info.x1 = x_next_i / subpixel_scale;
        glyph_info.y1 = pixels_height - 1 - (y_baseline + gbounds.y1 - pad_y);

        glyph_info.xoff = 0;
        glyph_info.yoff = -pad_y - gbounds.y2 + ascender_px;
        glyph_info.xadvance = (x_next - x) / subpixel_scale;

        if (subpixel_scale != 1 && glyph_info.x1 > glyph_info.x0) {
            glyph_lut_convolution(ren_buf, lcd_lut, cover_swap_buffer, glyph_info);
        }
        glyph_trim_rect(ren_buf, glyph_info, subpixel_scale);

        // When subpixel is activated we need one padding pixel on the left and on the right.
        x = x + gbounds.x2 + 2 * subpixel_scale;
    }
    delete [] index;
    delete [] bounds;
    delete [] cover_swap_buffer;

    std::string image_filename = fmt::format("{}-{}-{}.png", font_renderer->debug_font_name, first_char, font_height);
    fmt::print("{}\n", image_filename);

    FR_Bitmap *rgb_image = debug_bitmap_to_image_rgb(image, subpixel_scale);
    debug_rgb_image_write_glyphs_bbox(rgb_image, subpixel_scale, num_chars, glyphs, 0x00ff00);
    stbi_write_png(image_filename.c_str(), rgb_image->width, rgb_image->height, 3, rgb_image->pixels, rgb_image->width * 3);
    FR_Bitmap_Free(rgb_image);

    return image;
}

template <typename Order>
void blend_solid_hspan(agg::rendering_buffer& rbuf, int x, int y, unsigned len,
                        const agg::rgba8& c, const agg::int8u* covers)
{
    const int pixel_size = 4;
    agg::int8u* p = rbuf.row_ptr(y) + x * pixel_size;
    do
    {
        const unsigned alpha = *covers;
        const unsigned r = p[Order::R], g = p[Order::G], b = p[Order::B];
        p[Order::R] = (((unsigned(c.r) - r) * alpha) >> 8) + r;
        p[Order::G] = (((unsigned(c.g) - g) * alpha) >> 8) + g;
        p[Order::B] = (((unsigned(c.b) - b) * alpha) >> 8) + b;
        // Leave p[3], the alpha channel value unmodified.
        p += 4;
        ++covers;
    }
    while(--len);
}

template <typename Order>
void blend_solid_hspan_subpixel(agg::rendering_buffer& rbuf, agg::lcd_distribution_lut& lcd_lut,
    const int x, const int y, unsigned len,
    const agg::rgba8& c,
    const agg::int8u* covers)
{
    const int pixel_size = 4;
    const unsigned rgb[3] = { c.r, c.g, c.b };
    agg::int8u* p = rbuf.row_ptr(y) + x * pixel_size;

    // Indexes to adress RGB colors in a BGRA32 format.
    const int pixel_index[3] = {Order::R, Order::G, Order::B};
    for (unsigned cx = 0; cx < len; cx += 3)
    {
        for (int i = 0; i < 3; i++) {
            const unsigned cover_value = covers[cx + i];
            const unsigned alpha = (cover_value + 1) * (c.a + 1);
            const unsigned src_col = *(p + pixel_index[i]);
            *(p + pixel_index[i]) = (((rgb[i] - src_col) * alpha) + (src_col << 16)) >> 16;
        }
        // Leave p[3], the alpha channel value unmodified.
        p += 4;
    }
}

// destination implicitly BGRA32. Source implictly single-byte renderer_alpha coverage with subpixel scale = 3.
// FIXME: consider using something like RenColor* instead of uint8_t * for dst.
void FR_Blend_Glyph(FR_Renderer *font_renderer, FR_Clip_Area *clip, int x, int y, uint8_t *dst, int dst_width, const FR_Bitmap *glyphs_bitmap, const FR_Bitmap_Glyph_Metrics *glyph, FR_Color color) {
    agg::lcd_distribution_lut& lcd_lut = font_renderer->lcd_distribution_lut();
    const int subpixel_scale = font_renderer->subpixel_scale();
    const int pixel_size = 4; // Pixel size for BGRA32 format.

    x += glyph->xoff;
    y += glyph->yoff;

    int glyph_x = glyph->x0, glyph_y = glyph->y0;
    int glyph_width  = glyph->x1 - glyph->x0;
    int glyph_height = glyph->y1 - glyph->y0;

    int n;
    if ((n = clip->left - x) > 0) { glyph_width  -= n; glyph_x += n; x += n; }
    if ((n = clip->top  - y) > 0) { glyph_height -= n; glyph_y += n; y += n; }
    if ((n = x + glyph_width  - clip->right ) > 0) { glyph_width  -= n; }
    if ((n = y + glyph_height - clip->bottom) > 0) { glyph_height -= n; }

    if (glyph_width <= 0 || glyph_height <= 0) {
        return;
    }

    dst += (x + y * dst_width) * pixel_size;
    agg::rendering_buffer dst_ren_buf(dst, glyph_width, glyph_height, dst_width * pixel_size);

    uint8_t *src = glyphs_bitmap->pixels + (glyph_x + glyph_y * glyphs_bitmap->width) * subpixel_scale;
    int src_stride = glyphs_bitmap->width * subpixel_scale;

    const agg::rgba8 color_a(color.r, color.g, color.b);
    for (int x = 0, y = 0; y < glyph_height; y++) {
        agg::int8u *covers = src + y * src_stride;
        if (subpixel_scale == 1) {
            blend_solid_hspan<agg::order_bgra>(dst_ren_buf, x, y, glyph_width, color_a, covers);
        } else {
            blend_solid_hspan_subpixel<agg::order_bgra>(dst_ren_buf, lcd_lut, x, y, glyph_width * subpixel_scale, color_a, covers);
        }
    }
}

