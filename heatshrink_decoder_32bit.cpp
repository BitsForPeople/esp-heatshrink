#include "heatshrink_config.h"

// 32-bit optimized variant. Built if HEATSHRINK_32BIT is set.
#if HEATSHRINK_32BIT

#include <stdlib.h>
#include <string.h>
#include "heatshrink_decoder.h"



/* States for the polling state machine. */
typedef enum {
    HSDS_TAG_BIT,               /* tag bit */
    HSDS_YIELD_LITERAL,         /* ready to yield literal byte */
    HSDS_BACKREF_INDEX_MSB,     /* most significant byte of index */
    HSDS_BACKREF_INDEX_LSB,     /* least significant byte of index */
    HSDS_BACKREF_COUNT_MSB,     /* most significant byte of count */
    HSDS_BACKREF_COUNT_LSB,     /* least significant byte of count */
    HSDS_YIELD_BACKREF,         /* ready to yield back-reference */
} HSD_state;

#if HEATSHRINK_DEBUGGING_LOGS
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define ASSERT(X) assert(X)
static const char *state_names[] = {
    "tag_bit",
    "yield_literal",
    "backref_index_msb",
    "backref_index_lsb",
    "backref_count_msb",
    "backref_count_lsb",
    "yield_backref",
};
#else
#define LOG(...) /* no-op */
#define ASSERT(X) /* no-op */
#endif

typedef struct {
    uint8_t *buf;               /* output buffer */
    size_t buf_size;            /* buffer size */
    size_t *output_size;        /* bytes pushed to buffer, so far */
} output_info;

#define NO_BITS ((uint32_t)-1)

// static constexpr uint8_t BIT_INDEX_INIT = 

/* Forward references. */
static uint32_t get_bits(heatshrink_decoder *hsd, uint32_t count);
static bool no_bits(uint32_t bits);
static void push_byte(heatshrink_decoder *hsd, output_info *oi, uint32_t byte);

#if HEATSHRINK_DYNAMIC_ALLOC
heatshrink_decoder *heatshrink_decoder_alloc(uint16_t input_buffer_size,
                                             uint8_t window_sz2,
                                             uint8_t lookahead_sz2) {
    if ((window_sz2 < HEATSHRINK_MIN_WINDOW_BITS) ||
        (window_sz2 > HEATSHRINK_MAX_WINDOW_BITS) ||
        (input_buffer_size == 0) ||
        (lookahead_sz2 < HEATSHRINK_MIN_LOOKAHEAD_BITS) ||
        (lookahead_sz2 >= window_sz2)) {
        return NULL;
    }
    size_t buffers_sz = (1 << window_sz2) + input_buffer_size;
    size_t sz = sizeof(heatshrink_decoder) + buffers_sz;
    heatshrink_decoder *hsd = (heatshrink_decoder*) HEATSHRINK_MALLOC(sz);
    if (hsd == NULL) { return NULL; }
    hsd->input_buffer_size = input_buffer_size;
    hsd->window_sz2 = window_sz2;
    hsd->lookahead_sz2 = lookahead_sz2;
    heatshrink_decoder_reset(hsd);
    LOG("-- allocated decoder with buffer size of %zu (%zu + %u + %u)\n",
        sz, sizeof(heatshrink_decoder), (1 << window_sz2), input_buffer_size);
    return hsd;
}

void heatshrink_decoder_free(heatshrink_decoder *hsd) {
    size_t buffers_sz = (1 << hsd->window_sz2) + hsd->input_buffer_size;
    size_t sz = sizeof(heatshrink_decoder) + buffers_sz;
    HEATSHRINK_FREE(hsd, sz);
    (void)sz;   /* may not be used by free */
}
#endif

void heatshrink_decoder_reset(heatshrink_decoder *hsd) {
    size_t buf_sz = 1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd);
    size_t input_sz = HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd);
    memset(hsd->buffers, 0, buf_sz + input_sz);
    hsd->state = HSDS_TAG_BIT;
    hsd->input_size = 0;
    hsd->input_index = 0;
    hsd->bit_index = 0x00;
    hsd->current_byte = 0x00;
    hsd->output_count = 0;
    hsd->output_index = 0;
    hsd->head_index = 0;

// ESP_LOGI(TAG, "hsd: %" PRIu32 ", size: %" PRIu16, (uint32_t)hsd, hsd->input_size);

}

/* Copy SIZE bytes into the decoder's input buffer, if it will fit. */
HSD_sink_res heatshrink_decoder_sink(heatshrink_decoder *hsd,
        const uint8_t *in_buf, size_t size, size_t *input_size) {
    if ((hsd == NULL) || (in_buf == NULL) || (input_size == NULL)) {
        return HSDR_SINK_ERROR_NULL;
    }

// ESP_LOGI(TAG, "hsd: %" PRIu32 ", size: %" PRIu16, (uint32_t)hsd, hsd->input_size);

    size_t rem = HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd) - hsd->input_size;
    if (rem == 0) {
        *input_size = 0;
        return HSDR_SINK_FULL;
    }

    size = rem < size ? rem : size;
    LOG("-- sinking %zd bytes\n", size);
    /* copy into input buffer (at head of buffers) */
    memcpy(&hsd->buffers[hsd->input_size], in_buf, size);
    hsd->input_size += size;
    *input_size = size;
    return HSDR_SINK_OK;
}


/*****************
 * Decompression *
 *****************/

#define BACKREF_COUNT_BITS(HSD) (HEATSHRINK_DECODER_LOOKAHEAD_BITS(HSD))
#define BACKREF_INDEX_BITS(HSD) (HEATSHRINK_DECODER_WINDOW_BITS(HSD))

// States
static HSD_state st_tag_bit(heatshrink_decoder *hsd);
static HSD_state st_yield_literal(heatshrink_decoder *hsd,
    output_info *oi);
static HSD_state st_backref_index_msb(heatshrink_decoder *hsd);
static HSD_state st_backref_index_lsb(heatshrink_decoder *hsd);
static HSD_state st_backref_count_msb(heatshrink_decoder *hsd);
static HSD_state st_backref_count_lsb(heatshrink_decoder *hsd);
static HSD_state st_yield_backref(heatshrink_decoder *hsd,
    output_info *oi);

HSD_poll_res heatshrink_decoder_poll(heatshrink_decoder *hsd,
        uint8_t *out_buf, size_t out_buf_size, size_t *output_size) {
    if ((hsd == NULL) || (out_buf == NULL) || (output_size == NULL)) [[unlikely]] {
        return HSDR_POLL_ERROR_NULL;
    }
    *output_size = 0;

    output_info oi;
    oi.buf = out_buf;
    oi.buf_size = out_buf_size;
    oi.output_size = output_size;

    while (1) {
        LOG("-- poll, state is %d (%s), input_size %d\n",
            hsd->state, state_names[hsd->state], hsd->input_size);
        uint8_t in_state = hsd->state;
        switch (in_state) {
        case HSDS_TAG_BIT:
            hsd->state = st_tag_bit(hsd);
            break;
        case HSDS_YIELD_LITERAL:
            hsd->state = st_yield_literal(hsd, &oi);
            break;
        case HSDS_BACKREF_INDEX_MSB:
            hsd->state = st_backref_index_msb(hsd);
            break;
        case HSDS_BACKREF_INDEX_LSB:
            hsd->state = st_backref_index_lsb(hsd);
            break;
        case HSDS_BACKREF_COUNT_MSB:
            hsd->state = st_backref_count_msb(hsd);
            break;
        case HSDS_BACKREF_COUNT_LSB:
            hsd->state = st_backref_count_lsb(hsd);
            break;
        case HSDS_YIELD_BACKREF:
            hsd->state = st_yield_backref(hsd, &oi);
            break;
        default:
            return HSDR_POLL_ERROR_UNKNOWN;
        }
        
        /* If the current state cannot advance, check if input or output
         * buffer are exhausted. */
        if (hsd->state == in_state) {
            if (*output_size == out_buf_size) { return HSDR_POLL_MORE; }
            return HSDR_POLL_EMPTY;
        }
    }
}

static HSD_state st_tag_bit(heatshrink_decoder *hsd) {
    const uint32_t bits = get_bits(hsd, 1);  // get tag bit
    // if (bits == NO_BITS) {
    if(no_bits(bits)) {
        return HSDS_TAG_BIT;
    } else if (bits) {
        return HSDS_YIELD_LITERAL;
    } else if (HEATSHRINK_DECODER_WINDOW_BITS(hsd) > 8) {
        return HSDS_BACKREF_INDEX_MSB;
    } else {
        hsd->output_index = 0;
        return HSDS_BACKREF_INDEX_LSB;
    }
}

static HSD_state st_yield_literal(heatshrink_decoder *hsd,
        output_info *oi) {
    /* Emit a repeated section from the window buffer, and add it (again)
     * to the window buffer. (Note that the repetition can include
     * itself.)*/
    if (*oi->output_size < oi->buf_size) {
        const uint32_t byte = get_bits(hsd, 8);
        // if (byte == NO_BITS)
        if(no_bits(byte))
         { return HSDS_YIELD_LITERAL; } /* out of input */
        uint8_t* const buf = &hsd->buffers[HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd)];
        const uint32_t mask = (1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd))  - 1;
        // uint32_t c = byte & 0xFF;
        LOG("-- emitting literal byte 0x%02x ('%c')\n", c, isprint(c) ? c : '.');
        buf[hsd->head_index++ & mask] = byte;
        push_byte(hsd, oi, byte);
        return HSDS_TAG_BIT;
    } else {
        return HSDS_YIELD_LITERAL;
    }
}

static HSD_state st_backref_index_msb(heatshrink_decoder *hsd) {
    const uint32_t bit_ct = BACKREF_INDEX_BITS(hsd);
    ASSERT(bit_ct > 8);
    const uint32_t bits = get_bits(hsd, bit_ct - 8);
    LOG("-- backref index (msb), got 0x%04x (+1)\n", bits);
    // if (bits == NO_BITS) {
    if(no_bits(bits)) {        
         return HSDS_BACKREF_INDEX_MSB;
    } else {
        hsd->output_index = bits << 8;
        return HSDS_BACKREF_INDEX_LSB;
    }
}

static HSD_state st_backref_index_lsb(heatshrink_decoder *hsd) {
    const uint32_t bit_ct = BACKREF_INDEX_BITS(hsd);
    const uint32_t bits = get_bits(hsd, bit_ct < 8 ? bit_ct : 8);
    LOG("-- backref index (lsb), got 0x%04x (+1)\n", bits);
    // if (bits == NO_BITS) 
    if(no_bits(bits)) {
        return HSDS_BACKREF_INDEX_LSB;
    } else {
        hsd->output_index |= bits;
        hsd->output_index++;
        hsd->output_count = 0;    
        const uint32_t br_bit_ct = BACKREF_COUNT_BITS(hsd);
        return (br_bit_ct > 8) ? HSDS_BACKREF_COUNT_MSB : HSDS_BACKREF_COUNT_LSB;
    }
}

static HSD_state st_backref_count_msb(heatshrink_decoder *hsd) {
    const uint32_t br_bit_ct = BACKREF_COUNT_BITS(hsd);
    ASSERT(br_bit_ct > 8);
    const uint32_t bits = get_bits(hsd, br_bit_ct - 8);
    LOG("-- backref count (msb), got 0x%04x (+1)\n", bits);
    // if (bits == NO_BITS)
    if(no_bits(bits)) {
      return HSDS_BACKREF_COUNT_MSB;
    } else {
        hsd->output_count = bits << 8;
        return HSDS_BACKREF_COUNT_LSB;
    }
}

static HSD_state st_backref_count_lsb(heatshrink_decoder *hsd) {
    const uint32_t br_bit_ct = BACKREF_COUNT_BITS(hsd);
    const uint32_t bits = get_bits(hsd, br_bit_ct < 8 ? br_bit_ct : 8);
    LOG("-- backref count (lsb), got 0x%04x (+1)\n", bits);
    // if (bits == NO_BITS) { return HSDS_BACKREF_COUNT_LSB; }
    if(no_bits(bits)) {
        return HSDS_BACKREF_COUNT_LSB;
    } else {
        hsd->output_count |= bits;
        hsd->output_count++;
        return HSDS_YIELD_BACKREF;
    }
}

static HSD_state st_yield_backref(heatshrink_decoder *hsd,
        output_info *oi) {
    size_t count = oi->buf_size - *oi->output_size;
    if (count > 0) {
        // size_t i = 0;
        if (hsd->output_count < count) count = hsd->output_count;
        uint8_t* const buf = &hsd->buffers[HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd)];
        const uint32_t mask = (1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd)) - 1;
        // const uint32_t neg_offset = hsd->output_index;
        // LOG("-- emitting %zu bytes from -%u bytes back\n", count, neg_offset);
        // ASSERT(neg_offset <= mask + 1);
        ASSERT(count <= (size_t)(1 << BACKREF_COUNT_BITS(hsd)));

        {
            uint32_t di = hsd->head_index;
            const uint32_t dend = (di + count) & mask;
            uint32_t si = (di - hsd->output_index) & mask;
            // if(count >= 4 && dend > di && (si + count) <= mask ) {
            //     memmove(buf+di,buf+si,count);
            //     memcpy(oi->buf + *(oi->output_size), buf+si, count);
            //     *(oi->output_size) += count;
            //     di += count;
            // } else {
                uint8_t* out = oi->buf + *oi->output_size;
                do {
                    {
                    const uint32_t c = buf[si];
                    buf[di] = c;
                    // push_byte(hsd,oi,c);
                    *out = c;
                    }
                    ++out;                    
                    di = (di + 1) & mask;
                    si = (si + 1) & mask;
                } while( di != dend );
            // }
            *(oi->output_size) = out - oi->buf;
            hsd->head_index = di;
        }
        // for (i=0; i<count; i++) {
        //     uint8_t c = buf[(hsd->head_index - neg_offset) & mask];
        //     push_byte(hsd, oi, c);
        //     buf[hsd->head_index & mask] = c;
        //     hsd->head_index++;
        //     LOG("  -- ++ 0x%02x\n", c);
        // }
        hsd->output_count -= count;
        if (hsd->output_count == 0) { return HSDS_TAG_BIT; }
    }
    return HSDS_YIELD_BACKREF;
}

/* Get the next COUNT bits from the input buffer, saving incremental progress.
 * Returns NO_BITS on end of input, or if more than 15 bits are requested. */
static uint32_t get_bits(heatshrink_decoder *hsd, uint32_t count) {
    // uint16_t accumulator = 0;

    // int i = 0;
    // In fact, count is always <= 8.
    if (count > 15) [[unlikely]] { return NO_BITS; }
    LOG("-- popping %u bit(s)\n", count);

    /* If we aren't able to get COUNT bits, suspend immediately, because we
     * don't track how many bits of COUNT we've accumulated before suspend. */
    // if (hsd->bit_index < count && hsd->input_size == 0) { return NO_BITS; }


    {

        uint32_t bi = hsd->bit_index;

        if(bi < count) {
            // Because count is never >8, i.e. 1 byte, this check is ok:
            if( hsd->input_size == 0 ) {
                return NO_BITS;
            }
        }
        // {
        //     // if(bi < count && (count - bi) > (((uint32_t)hsd->input_size - hsd->input_index)*8)) {
        //     //     return NO_BITS;
        //     // }
        //     const uint32_t bitsAvailable = bi + (((uint32_t)hsd->input_size - hsd->input_index)*8);
        //     if(bitsAvailable < count) { return NO_BITS; }
        // }        

        uint32_t r = 0;
        uint32_t cb = hsd->current_byte;
        do {
            {
                const uint32_t c = count < bi ? count : bi;
                if(c != 0) {
                    r = (r << c) | ((cb>>(bi-c)) & ((1<<c)-1));
                    count -= c;
                    bi -= c;
                }
            }
            if(count != 0) {
                uint32_t ii = (uint32_t)hsd->input_index;
                cb = hsd->buffers[ii];
                hsd->current_byte = cb;
                bi = 8;
                // Advance input pointer
                ii += 1;
                hsd->input_index = ii;
                if (ii == hsd->input_size) [[unlikely]] {
                    hsd->input_index = 0; /* input is exhausted */
                    hsd->input_size = 0;
                }    
            }
        } while( count != 0 /*&& bi != 0*/);

        hsd->bit_index = bi;

        return r;
    }
    // /* If we aren't able to get COUNT bits, suspend immediately, because we
    //  * don't track how many bits of COUNT we've accumulated before suspend. */
    // if (hsd->input_size == 0) {
    //     if (hsd->bit_index < (1 << (count - 1))) { return NO_BITS; }
    // }

    // for (i = 0; i < count; i++) {
    //     if (hsd->bit_index == 0x00) {
    //         if (hsd->input_size == 0) {
    //             LOG("  -- out of bits, suspending w/ accumulator of %u (0x%02x)\n",
    //                 accumulator, accumulator);
    //             return NO_BITS;
    //         }
    //         hsd->current_byte = hsd->buffers[hsd->input_index++];
    //         LOG("  -- pulled byte 0x%02x\n", hsd->current_byte);
    //         if (hsd->input_index == hsd->input_size) {
    //             hsd->input_index = 0; /* input is exhausted */
    //             hsd->input_size = 0;
    //         }
    //         hsd->bit_index = 0x80;
    //     }
    //     accumulator <<= 1;
    //     if (hsd->current_byte & hsd->bit_index) {
    //         accumulator |= 0x01;
    //         if (0) {
    //             LOG("  -- got 1, accumulator 0x%04x, bit_index 0x%02x\n",
    //             accumulator, hsd->bit_index);
    //         }
    //     } else {
    //         if (0) {
    //             LOG("  -- got 0, accumulator 0x%04x, bit_index 0x%02x\n",
    //             accumulator, hsd->bit_index);
    //         }
    //     }
    //     hsd->bit_index >>= 1;
    // }

    // if (count > 1) { LOG("  -- accumulated %08x\n", accumulator); }
    // return accumulator;
}

static bool no_bits(uint32_t bits) {
    return (int32_t)bits < 0;
}

HSD_finish_res heatshrink_decoder_finish(heatshrink_decoder *hsd) {
    if (hsd == NULL) { return HSDR_FINISH_ERROR_NULL; }
    switch (hsd->state) {
    case HSDS_TAG_BIT:
        return hsd->input_size == 0 ? HSDR_FINISH_DONE : HSDR_FINISH_MORE;

    /* If we want to finish with no input, but are in these states, it's
     * because the 0-bit padding to the last byte looks like a backref
     * marker bit followed by all 0s for index and count bits. */
    case HSDS_BACKREF_INDEX_LSB:
    case HSDS_BACKREF_INDEX_MSB:
    case HSDS_BACKREF_COUNT_LSB:
    case HSDS_BACKREF_COUNT_MSB:
        return hsd->input_size == 0 ? HSDR_FINISH_DONE : HSDR_FINISH_MORE;

    /* If the output stream is padded with 0xFFs (possibly due to being in
     * flash memory), also explicitly check the input size rather than
     * uselessly returning MORE but yielding 0 bytes when polling. */
    case HSDS_YIELD_LITERAL:
        return hsd->input_size == 0 ? HSDR_FINISH_DONE : HSDR_FINISH_MORE;

    default:
        return HSDR_FINISH_MORE;
    }
}

static void push_byte(heatshrink_decoder *hsd, output_info *oi, uint32_t byte) {
    LOG(" -- pushing byte: 0x%02x ('%c')\n", byte, isprint(byte) ? byte : '.');
    oi->buf[(*oi->output_size)++] = byte;
    // (void)hsd;
}

#endif // HEATSHRINK_32BIT