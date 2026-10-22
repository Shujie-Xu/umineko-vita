#include "png_stream.h"
#include <png.h>
#include <stdlib.h>

struct OnsVitaPngStream {
    png_structp png;
    png_infop info;
    FILE *fp;
};

static void read_data(png_structp png, png_bytep data, png_size_t size) {
    OnsVitaPngStream *stream = (OnsVitaPngStream *)png_get_io_ptr(png);
    if (fread(data, 1, size, stream->fp) != size)
        png_error(png, "Truncated PNG stream");
}

static void destroy(OnsVitaPngStream *stream) {
    png_destroy_read_struct(&stream->png, stream->info ? &stream->info : NULL, NULL);
    free(stream);
}

OnsVitaPngStream *ons_vita_png_open(FILE *fp, uint32_t *width, uint32_t *height) {
    OnsVitaPngStream *volatile stream = calloc(1, sizeof(OnsVitaPngStream));
    if (!stream)
        return NULL;
    stream->fp = fp;
    stream->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (stream->png)
        stream->info = png_create_info_struct(stream->png);
    if (!stream->png || !stream->info) {
        destroy(stream);
        return NULL;
    }
    if (setjmp(png_jmpbuf(stream->png))) {
        destroy(stream);
        return NULL;
    }
    png_set_read_fn(stream->png, stream, read_data);
    png_read_info(stream->png, stream->info);
    png_uint_32 w = 0, h = 0;
    int depth = 0, color = 0, interlace = 0;
    png_get_IHDR(stream->png, stream->info, &w, &h, &depth, &color, &interlace, NULL, NULL);
    if (!w || !h || depth != 8 || color != PNG_COLOR_TYPE_RGB || interlace != PNG_INTERLACE_NONE ||
        png_get_valid(stream->png, stream->info, PNG_INFO_tRNS)) {
        destroy(stream);
        return NULL;
    }
    png_read_update_info(stream->png, stream->info);
    *width = w;
    *height = h;
    return stream;
}

int ons_vita_png_read_rows(OnsVitaPngStream *stream, unsigned char *pixels, size_t pitch, uint32_t rows) {
    if (setjmp(png_jmpbuf(stream->png)))
        return -1;
    for (uint32_t row = 0; row < rows; ++row)
        png_read_row(stream->png, pixels + row * pitch, NULL);
    return 0;
}

void ons_vita_png_close(OnsVitaPngStream *stream) {
    if (stream)
        destroy(stream);
}
