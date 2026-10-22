#ifndef ONS_VITA_PNG_STREAM_H
#define ONS_VITA_PNG_STREAM_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Row-at-a-time PNG decoding for images uploaded in tiles (GPUBigImage), so
 * neither the compressed file nor the decoded image is held in memory whole. */
typedef struct OnsVitaPngStream OnsVitaPngStream;

/* Reads the header from fp (which stays owned by the caller). Accepts only
 * 8-bit RGB, non-interlaced PNGs without tRNS: their rows are the exact bytes
 * the engine's PNG loader would produce. Returns NULL for anything else. */
OnsVitaPngStream *ons_vita_png_open(FILE *fp, uint32_t *width, uint32_t *height);
/* Decodes the next rows into lines pitch bytes apart. Returns 0 on success. */
int ons_vita_png_read_rows(OnsVitaPngStream *stream, unsigned char *pixels, size_t pitch, uint32_t rows);
void ons_vita_png_close(OnsVitaPngStream *stream);
#endif
