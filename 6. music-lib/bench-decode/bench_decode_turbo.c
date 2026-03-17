#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <jpeglib.h>
#include <png.h>

#define RUNS 100

static unsigned char *decode_jpeg(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, f);
	jpeg_read_header(&cinfo, 1);
	cinfo.out_color_space = JCS_RGB;
	jpeg_start_decompress(&cinfo);
	*w = cinfo.output_width;
	*h = cinfo.output_height;
	int stride = *w * 3;
	unsigned char *buf = malloc(stride * *h);
	while (cinfo.output_scanline < cinfo.output_height) {
		unsigned char *row = buf + cinfo.output_scanline * stride;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	fclose(f);
	return buf;
}

static unsigned char *decode_png(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png_create_info_struct(png);
	png_init_io(png, f);
	png_read_info(png, info);
	*w = png_get_image_width(png, info);
	*h = png_get_image_height(png, info);
	png_byte ct = png_get_color_type(png, info);
	png_byte bd = png_get_bit_depth(png, info);
	if (bd == 16) png_set_strip_16(png);
	if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
	if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
	if (ct == PNG_COLOR_TYPE_RGBA || ct == PNG_COLOR_TYPE_GA)
		png_set_strip_alpha(png);
	if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GA)
		png_set_gray_to_rgb(png);
	png_read_update_info(png, info);
	int stride = *w * 3;
	unsigned char *buf = malloc(stride * *h);
	png_bytep *rows = malloc(*h * sizeof(png_bytep));
	for (int y = 0; y < *h; y++) rows[y] = buf + y * stride;
	png_read_image(png, rows);
	free(rows);
	png_destroy_read_struct(&png, &info, NULL);
	fclose(f);
	return buf;
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s <png> <jpg>\n", argv[0]); return 1; }

	int w, h;

	/* warm up */
	free(decode_png(argv[1], &w, &h));
	free(decode_jpeg(argv[2], &w, &h));

	struct timespec t0, t1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < RUNS; i++) free(decode_png(argv[1], &w, &h));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double png_ms = ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9) * 1000;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < RUNS; i++) free(decode_jpeg(argv[2], &w, &h));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double jpg_ms = ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9) * 1000;

	printf("PNG (libpng):        %dx%d, %d runs, %.1f ms total, %.2f ms/decode\n", w, h, RUNS, png_ms, png_ms / RUNS);
	printf("JPG (libjpeg-turbo): %dx%d, %d runs, %.1f ms total, %.2f ms/decode\n", w, h, RUNS, jpg_ms, jpg_ms / RUNS);
	printf("Ratio: PNG/JPG = %.2fx\n", png_ms / jpg_ms);
	return 0;
}
