#ifndef LIBQUA_H
#define LIBQUA_H

#ifdef __cplusplus
extern "C" {
#endif

// This attribute tells the compiler to make this function public
__attribute__((visibility("default")))
char* find_biggest_art(const char *input_path);

#ifdef __cplusplus
}
#endif

#endif
