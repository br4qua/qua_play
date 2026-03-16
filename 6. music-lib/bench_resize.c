#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RUNS 50

/* simple box filter downscale — 3 channel RGB */
static void box_downscale(const unsigned char *src, int sw, int sh,
			  unsigned char *dst, int dw, int dh)
{
	float sx = (float)sw / dw;
	float sy = (float)sh / dh;
	for (int y = 0; y < dh; y++) {
		int y0 = (int)(y * sy);
		int y1 = (int)((y + 1) * sy);
		if (y1 > sh) y1 = sh;
		for (int x = 0; x < dw; x++) {
			int x0 = (int)(x * sx);
			int x1 = (int)((x + 1) * sx);
			if (x1 > sw) x1 = sw;
			unsigned r = 0, g = 0, b = 0, n = 0;
			for (int yy = y0; yy < y1; yy++) {
				const unsigned char *row = src + yy * sw * 3;
				for (int xx = x0; xx < x1; xx++) {
					r += row[xx * 3];
					g += row[xx * 3 + 1];
					b += row[xx * 3 + 2];
					n++;
				}
			}
			dst[(y * dw + x) * 3]     = r / n;
			dst[(y * dw + x) * 3 + 1] = g / n;
			dst[(y * dw + x) * 3 + 2] = b / n;
		}
	}
}

int main(void)
{
	int w, h, ch;
	unsigned char *img = stbi_load("test_cover.png", &w, &h, &ch, 3);
	if (!img) { fprintf(stderr, "failed\n"); return 1; }

	int nw = 1200, nh = 1200;
	if (w <= 1200 && h <= 1200) {
		printf("Image %dx%d already fits, need a bigger test image\n", w, h);
		stbi_image_free(img);
		return 1;
	}
	double s = 1200.0 / (w > h ? w : h);
	nw = (int)(w * s);
	nh = (int)(h * s);
	printf("Resize: %dx%d -> %dx%d\n", w, h, nw, nh);

	unsigned char *out = malloc(nw * nh * 3);
	struct timespec t0, t1;

	/* warm up */
	stbir_resize_uint8_linear(img, w, h, 0, out, nw, nh, 0, STBIR_RGB);
	box_downscale(img, w, h, out, nw, nh);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < RUNS; i++)
		stbir_resize_uint8_linear(img, w, h, 0, out, nw, nh, 0, STBIR_RGB);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double stb_ms = ((t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9)*1000;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < RUNS; i++)
		box_downscale(img, w, h, out, nw, nh);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double box_ms = ((t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9)*1000;

	printf("stb_image_resize2:  %d runs, %.1f ms total, %.2f ms/resize\n", RUNS, stb_ms, stb_ms/RUNS);
	printf("box filter:         %d runs, %.1f ms total, %.2f ms/resize\n", RUNS, box_ms, box_ms/RUNS);
	printf("Ratio: stb/box = %.2fx\n", stb_ms / box_ms);

	free(out);
	stbi_image_free(img);
	return 0;
}
