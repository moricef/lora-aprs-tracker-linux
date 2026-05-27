#pragma once
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_SPIRAM 0
inline size_t heap_caps_get_free_size(int) { return 1024*1024; }
inline size_t heap_caps_get_largest_free_block(int) { return 512*1024; }
