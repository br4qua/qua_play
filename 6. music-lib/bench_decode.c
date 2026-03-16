#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <stdio.h>
#include <time.h>

#define RUNS 100

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s <image>\n", argv[0]); return 1; }

	/* warm up page cache */
	int w, h, ch;
	unsigned char *img = stbi_load(argv[1], &w, &h, &ch, 3);
	if (!img) { fprintf(stderr, "failed to load %s\n", argv[1]); return 1; }
	stbi_image_free(img);

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < RUNS; i++) {
		img = stbi_load(argv[1], &w, &h, &ch, 3);
		stbi_image_free(img);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("%s: %dx%d, %d runs, %.1f ms total, %.2f ms/decode\n",
	       argv[1], w, h, RUNS, elapsed * 1000, elapsed * 1000 / RUNS);
	return 0;
}
