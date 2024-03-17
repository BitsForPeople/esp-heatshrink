# heatshrink for ESP32
This is a modification of the heatshrink data compression library intended for use on ESP32 MCUs,
and an experiment/proof-of-concept for the use of the ESP32-S3's SIMD instruction set ("Processor 
Instruction Extensions", "PIE") in data compression.

The original heatshrink is made into a component for use in ESP-IDF builds and speed-optimized for
32 bit architectures like the ESP32s', including new and faster pattern-match search functions.

The new search functions take advantage of Xtensa's "zero-overhead loops" when built for supporting
Xtensa-based ESP32 MCUs (ESP32, ESP32-S3).

On the **ESP32-S3**, the MCU's SIMD instructions ("PIE") are used which further speeds up compression
by a factor of _a lot_.

Use of the 32-bit optimized variant can be enabled/disabled by setting `HEATSHRINK_32BIT` to 1 or 0
in `heatshrink_config.h`; then, if built via ESP-IDF, ESP32 optimizations are automatically
enabled based on the build's target SoC, with plain C/C++ fallbacks in place for non-Xtensa platforms.

## Exemplary benchmarks

### ESP32-S3 (Xtensa)

Heatshrink **(12,4)**, compressing 5614 bytes of text down to 2635 bytes:

| Variant | CPU cycles | Time @ 240MHz | Time (relative) | Speed (relative) |
|--|--|--|--|--|
| Original (C) | 100829896 | 420,1 ms | 100,0 % | 1,0 x |
| 32-bit, New Search (C/C++) | 38911824 | 162,1 ms | 38,6 % | 2,6 x |
| Xtensa optimized (C/C++/inl. asm.) | 34488868 | 143,7 ms | 34,2 % | 2,9 x |
| ESP32-S3 SIMD (C/C++/inl. asm.) | 5248242 | 21,9 ms | 5,2 % | 19,2 x |
| Original w/ USE_INDEX(*) (C) | 3636812 | 15,2 ms | 3,6 % | 27,7 x |

Heatshrink **(10,4)**, compressing 5614 bytes of text down to 2831 bytes:

| Variant | CPU cycles | Time @ 240MHz | Time (relative) | Speed (relative) |
|--|--|--|--|--|
| Original (C) | 27143074 | 113,1 ms | 100,0 % | 1,0 x |
| 32-bit, New Search (C/C++) | 10981088 | 45,8 ms | 40,5 % | 2,5 x |
| Xtensa optimized (C/C++/inl. asm.) | 9744568 | 40,6 ms | 35,9 % | 2,8 x |
| ESP32-S3 SIMD (C/C++/inl. asm.) | 2000388 | 8,3 ms | 7,4 % | 13,6 x |
| Original w/ USE_INDEX(*) (C) | 2044378 | 8,5 ms | 7,5 % | 13,3 x |

### ESP32-C3 (RISC-V)

Heatshrink **(12,4)**, compressing 5614 bytes of text down to 2635 bytes:

| Variant | CPU cycles | Time @ 160MHz | Time (relative) | Speed (relative) |
|--|--|--|--|--|
| Original (C) | 100778552 | 629,9 ms | 100,0 % | 1,0 x |
| 32-bit, New Search (C/C++) | 54724768 | 342,0 ms | 54,3 % | 1,8 x |
| Original w/ USE_INDEX(*) (C) | 3495174 | 21,8 ms | 3,5 % | 28,8 x |

Heatshrink **(10,4)**, compressing 5614 bytes of text down to 2831 bytes:

| Variant | CPU cycles | Time @ 160MHz | Time (relative) | Speed (relative) |
|--|--|--|--|--|
| Original (C) | 27101012 | 169,4 ms | 100,0 % | 1,0 x |
| 32-bit, New Search (C/C++) | 14528170 | 90,8 ms | 53,6 % | 1,9 x |
| Original w/ USE_INDEX(*) (C) | 1997088 | 12,5 ms | 7,4 % | 13,6 x |

(*) HEATSHRINK_USE_INDEX increases RAM requirement for compression by ~3x

Obviously, YMMV as the possible performance gain also depends on the actual data being
compressed; 'harder' to compress -> more potential performance gain.

## Code

The original heatshrink encoder is in `heatshrink_encoder.c` (used if `HEATSHRINK_32BIT`
is set to 0) to make it easy to build and compare variants vs. the original.

`heatshrink_encoder_32bit.cpp` contains some optimizations and is built if `HEATSHRINK_32BIT`
is set to 1. The actual heavy lifting of this variant is done by the 32-bit/SIMD optimized
search functions which live in `private/hs_search.hpp`.

## Note
1) The 32-bit modifications require the target architecture to support unaligned 32-bit reads
from the memory buffer used by the encoder.
2) Heatshrink is based on LZSS compression, which means heatshrink alone can only compress
repeating sequences of bytes in a data stream. This makes it a reasonable choice for
compressing text, HTML, or some types of bitmap images but not so much for analog signals,
sensor readings, or photos, because even a small amount of noise may eliminate most or all
repetitions of identical byte sequences.


Heatshrink's original Readme below:

# heatshrink

A data compression/decompression library for embedded/real-time systems.


## Key Features:

- **Low memory usage (as low as 50 bytes)**
    It is useful for some cases with less than 50 bytes, and useful
    for many general cases with < 300 bytes.
- **Incremental, bounded CPU use**
    You can chew on input data in arbitrarily tiny bites.
    This is a useful property in hard real-time environments.
- **Can use either static or dynamic memory allocation**
    The library doesn't impose any constraints on memory management.
- **ISC license**
    You can use it freely, even for commercial purposes.


## Getting Started:

There is a standalone command-line program, `heatshrink`, but the
encoder and decoder can also be used as libraries, independent of each
other. To do so, copy `heatshrink_common.h`, `heatshrink_config.h`, and
either `heatshrink_encoder.c` or `heatshrink_decoder.c` (and their
respective header) into your project. For projects that use both,
static libraries are built that use static and dynamic allocation.

Dynamic allocation is used by default, but in an embedded context, you
probably want to statically allocate the encoder/decoder. Set
`HEATSHRINK_DYNAMIC_ALLOC` to 0 in `heatshrink_config.h`.


### Basic Usage

1. Allocate a `heatshrink_encoder` or `heatshrink_decoder` state machine
using their `alloc` function, or statically allocate one and call their
`reset` function to initialize them. (See below for configuration
options.)

2. Use `sink` to sink an input buffer into the state machine. The
`input_size` pointer argument will be set to indicate how many bytes of
the input buffer were actually consumed. (If 0 bytes were conusmed, the
buffer is full.)

3. Use `poll` to move output from the state machine into an output
buffer. The `output_size` pointer argument will be set to indicate how
many bytes were output, and the function return value will indicate
whether further output is available. (The state machine may not output
any data until it has received enough input.)

Repeat steps 2 and 3 to stream data through the state machine. Since
it's doing data compression, the input and output sizes can vary
significantly. Looping will be necessary to buffer the input and output
as the data is processed.

4. When the end of the input stream is reached, call `finish` to notify
the state machine that no more input is available. The return value from
`finish` will indicate whether any output remains. if so, call `poll` to
get more.

Continue calling `finish` and `poll`ing to flush remaining output until
`finish` indicates that the output has been exhausted.

Sinking more data after `finish` has been called will not work without
calling `reset` on the state machine.


## Configuration

heatshrink has a couple configuration options, which impact its resource
usage and how effectively it can compress data. These are set when
dynamically allocating an encoder or decoder, or in `heatshrink_config.h`
if they are statically allocated.

- `window_sz2`, `-w` in the CLI: Set the window size to 2^W bytes.

The window size determines how far back in the input can be searched for
repeated patterns. A `window_sz2` of 8 will only use 256 bytes (2^8),
while a `window_sz2` of 10 will use 1024 bytes (2^10). The latter uses
more memory, but may also compress more effectively by detecting more
repetition.

The `window_sz2` setting currently must be between 4 and 15.

- `lookahead_sz2`, `-l` in the CLI: Set the lookahead size to 2^L bytes.

The lookahead size determines the max length for repeated patterns that
are found. If the `lookahead_sz2` is 4, a 50-byte run of 'a' characters
will be represented as several repeated 16-byte patterns (2^4 is 16),
whereas a larger `lookahead_sz2` may be able to represent it all at
once. The number of bits used for the lookahead size is fixed, so an
overly large lookahead size can reduce compression by adding unused
size bits to small patterns.

The `lookahead_sz2` setting currently must be between 3 and the
`window_sz2` - 1.

- `input_buffer_size` - How large an input buffer to use for the
decoder. This impacts how much work the decoder can do in a single
step, and a larger buffer will use more memory. An extremely small
buffer (say, 1 byte) will add overhead due to lots of suspend/resume
function calls, but should not change how well data compresses.


### Recommended Defaults

For embedded/low memory contexts, a `window_sz2` in the 8 to 10 range is
probably a good default, depending on how tight memory is. Smaller or
larger window sizes may make better trade-offs in specific
circumstances, but should be checked with representative data.

The `lookahead_sz2` should probably start near the `window_sz2`/2, e.g.
-w 8 -l 4 or -w 10 -l 5. The command-line program can be used to measure
how well test data works with different settings.


## More Information and Benchmarks:

heatshrink is based on [LZSS], since it's particularly suitable for
compression in small amounts of memory. It can use an optional, small
[index] to make compression significantly faster, but otherwise can run
in under 100 bytes of memory. The index currently adds 2^(window size+1)
bytes to memory usage for compression, and temporarily allocates 512
bytes on the stack during index construction (if the index is enabled).

For more information, see the [blog post] for an overview, and the
`heatshrink_encoder.h` / `heatshrink_decoder.h` header files for API
documentation.

[blog post]: http://spin.atomicobject.com/2013/03/14/heatshrink-embedded-data-compression/
[index]: http://spin.atomicobject.com/2014/01/13/lightweight-indexing-for-embedded-systems/
[LZSS]: http://en.wikipedia.org/wiki/Lempel-Ziv-Storer-Szymanski


## Build Status

  [![Build Status](https://travis-ci.org/atomicobject/heatshrink.png)](http://travis-ci.org/atomicobject/heatshrink)
