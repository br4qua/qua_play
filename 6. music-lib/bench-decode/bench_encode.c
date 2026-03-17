#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <png.h>

#define RUNS 20

static void encode_stbi(unsigned char *rgb, int w, int h)
{
	stbi_write_png("/dev/null", w, h, 3, rgb, w * 3);
}

static void encode_libpng(unsigned char *rgb, int w, int h)
{
	FILE *f = fopen("/dev/null", "wb");
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png_create_info_struct(png);
	png_init_io(png, f);
	png_set_compression_level(png, 1); /* fast */
	png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB,
		     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
		     PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	for (int y = 0; y < h; y++)
		png_write_row(png, rgb + y * w * 3);
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	fclose(f);
}

static void encode_libpng_default(unsigned char *rgb, int w, int h)
{
	FILE *f = fopen("/dev/null", "wb");
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png_create_info_struct(png);
	png_init_io(png, f);
	png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB,
		     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
		     PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	for (int y = 0; y < h; y++)
		png_write_row(png, rgb + y * w * 3);
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	fclose(f);
}

#define BENCH(label, fn) do { \
	clock_gettime(CLOCK_MONOTONIC, &t0); \
	for (int i = 0; i < RUNS; i++) fn(rgb, w, h); \
	clock_gettime(CLOCK_MONOTONIC, &t1); \
	double ms = ((t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9)*1000; \
	printf("%-25s %d runs, %.1f ms total, %.2f ms/encode\n", label, RUNS, ms, ms/RUNS); \
} while(0)

int main(void)
{
	int w, h, ch;
	unsigned char *rgb = stbi_load("test_cover.png", &w, &h, &ch, 3);
	if (!rgb) { fprintf(stderr, "failed to load\n"); return 1; }
	printf("Image: %dx%d\n", w, h);

	struct timespec t0, t1;
	BENCH("stbi_write_png:", encode_stbi);
	BENCH("libpng (level 1):", encode_libpng);
	BENCH("libpng (default):", encode_libpng_default);

	stbi_image_free(rgb);
	return 0;
}
