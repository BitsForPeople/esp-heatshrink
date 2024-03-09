#ifndef HEATSHRINK_CONFIG_H
#define HEATSHRINK_CONFIG_H

/* Should functionality assuming dynamic allocation be used? */
#ifndef HEATSHRINK_DYNAMIC_ALLOC
#define HEATSHRINK_DYNAMIC_ALLOC 1
#endif

/* Should 32 bit variables be used for computations in the encoder? (Faster on 32 bit architectures.) */
#define HEATSHRINK_32BIT 1

/* Should a (32 bit) optimized pattern search implementation be used? (Faster on 32 bit architectures.) */
#define HEATSHRINK_NEW_SEARCH 1

#if HEATSHRINK_DYNAMIC_ALLOC
    /* Optional replacement of malloc/free */
    #define HEATSHRINK_MALLOC(SZ) malloc(SZ)
    #define HEATSHRINK_FREE(P, SZ) free(P)
#else
    /* Required parameters for static configuration */
    #define HEATSHRINK_STATIC_INPUT_BUFFER_SIZE 32
    #define HEATSHRINK_STATIC_WINDOW_BITS 8
    #define HEATSHRINK_STATIC_LOOKAHEAD_BITS 4
#endif

/* Turn on logging for debugging. */
#define HEATSHRINK_DEBUGGING_LOGS 0

/* Use indexing for faster compression. (Increases RAM requirement by ~200%, cannot be used 
   together with HEATSHRINK_NEW_SEARCH.) */
#define HEATSHRINK_USE_INDEX 0

#endif
