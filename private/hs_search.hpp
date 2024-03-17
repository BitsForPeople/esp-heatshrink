/*

    Copyright 2023, <https://github.com/BitsForPeople>
     
    This program is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the
    Free Software Foundation, either version 3 of the License, or (at your
    option) any later version.

    This program is distributed in the hope that it will be useful, bu
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License along
    with this program. If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include <cstdint>
#if __cplusplus > 201703L
#include <span>
#endif
#include <type_traits>
#include <algorithm>

#include "hs_arch.hpp"

namespace heatshrink {

    #if __cpp_lib_span >= 202002L
    using byte_span = std::span<const uint8_t>;
    #else
    class byte_span {
        const uint8_t* m_data {nullptr};
        std::size_t m_len {0};
        public:
        constexpr byte_span() noexcept = default;
        constexpr byte_span(const byte_span&) noexcept = default;
        constexpr byte_span(const uint8_t* const data, const std::size_t len) :
            m_data {data},
            m_len {len} {

            }

        constexpr byte_span& operator =(const byte_span&) noexcept = default;

        constexpr const uint8_t* data() const noexcept {
            return m_data;
        }

        constexpr bool empty() const noexcept {
            return m_len == 0;
        }

        constexpr std::size_t size() const noexcept {
            return m_len;
        }

        constexpr std::size_t size_bytes() const noexcept {
            return m_len;
        }
    };
    #endif

    /**
     * @brief Optimized functions for locating substring ("pattern") matches.
     *
     * These are heatshrink-specific versions because for heatshrink a match only needs
     * to \e start inside the given region of memory ("data") and may extend beyond that.
     *
     */
    class Locator {
        private:
            /*
              We multiply (mac) this vector with itself, then mac the values at all
              even positions again which makes the even positions 'worth' 2x as much,
              so we effectively get an 8x16 mac with
              {2*(128*128),(128*128),2*(64*64),(64*64),...,2*(1*1),(1*1)}, i.e.
              {(1<<15),(1<<14),(1<<13),...,(1<<1),(1<<0)} as desired.

              Positions in the vector are mapped to bits in descending order, the first
              u8 to 1<<15, the last u8 to 1<<0. This is because the Xtensas have a 1-cycle
              "count leading zeroes" (CLZ) instruction ("NSAU"), whereas a "count trailing
              zeroes" (CTZ) or "find first set" (FFS) requires ~4 instructions.
             */
            /**
             * @brief Vector of \c uint8_t used for extracting match positions when
             * using ESP32-S3 SIMD.
             */
            alignas(16) static constexpr uint8_t POS_U8[16] {
                128,128,64,64,32,32,16,16,8,8,4,4,2,2,1,1,
            };


            /**
             * @brief Under the precondition that *(uint32_t*)a != *(uint32_t*)b, returns the
             * common prefix length of *a and *b in bytes (0...3)
             *
             * @param a
             * @param b
             * @return common prefix length of *a and *b in bytes (0...3)
             */
            static uint32_t subword_match_len(const void* a, const void* b) noexcept {
                if constexpr (false && Arch::XTENSA) {
                    // uint32_t tmp = (uint32_t)as<uint16_t>(a) - as<uint16_t>(b);
                    // tmp = sizeof(uint16_t) * (1-std::min(tmp,(uint32_t)1));
                    // tmp += 1-std::min((uint32_t)(p<uint8_t>(a)[tmp]) - p<uint8_t>(b)[tmp],(uint32_t)1);
                    // return tmp;
                    // Branchless, but slower on an ESP32-S3.
                    uint32_t tmp1 = (uint32_t)as<uint16_t>(a) - as<uint16_t>(b);
                    tmp1 = (tmp1 == 0) ? 2 : 0;
                    uint32_t tmp2 = (uint32_t)(p<uint8_t>(a)[tmp1]) - p<uint8_t>(b)[tmp1];
                    return tmp1 + ((tmp2 == 0) ? 1 : 0);
                } else {
                    if(as<uint16_t>(a) == as<uint16_t>(b)) {
                        // First 2 bytes match
                        if(p<uint8_t>(a)[2] == p<uint8_t>(b)[2]) {
                            // Byte #3 also matches -> mismatch must be in byte #4
                            return 3;
                        } else {
                            // Mismatch in byte #3
                            return 2;
                        }
                    } else {
                        // Mismatch in the first two bytes
                        if(p<uint8_t>(a)[0] == p<uint8_t>(b)[0]) {
                            // Byte #1 matches -> mismatch must be in byte #2
                            return 1;
                        } else {
                            // Mismatch in first byte
                            return 0;
                        }
                    }
                }
                // const uint16_t* pa = (const uint16_t*)a;
                // const uint16_t* pb = (const uint16_t*)b;
                // uint32_t x = (pa[0] == pb[0]) << 1;
                // x += ((const uint8_t*)a)[x] == ((const uint8_t*)b)[x];
                // return x;
            }

            template<int32_t INC, typename T>
            static void __attribute__((always_inline)) incptr(T*& ptr) noexcept {
                ptr = (T*)(((uintptr_t)ptr) + INC);
            }

            template<typename T>
            static void __attribute__((always_inline)) incptr(T*& ptr, const int32_t inc) noexcept {
                ptr = (T*)(((uintptr_t)ptr) + inc);
            }            

            template<typename T>
            static const T* p(const void* const ptr) noexcept {
                return (const T*)ptr;
            }

            template<typename T, int32_t INC>
            static const T* pplus(const void* const ptr) noexcept {
                return (const T*)((uintptr_t)ptr + INC);
            }

            template<typename T>
            static T __attribute__((always_inline)) as(const void* const ptr) noexcept {
                return *reinterpret_cast<const T*>(ptr);
            }

            /**
             * @brief Scalar pattern search for patterns <= sizeof(uint32_t) bytes in length.
             *
             * @param pattern
             * @param patLen must be <= sizeof(uint32_t)
             * @param data
             * @param dataLen
             * @return start of the first match found in \p data, or \c nullptr if none found.
             */
            // For some reason, without noinline we get only about HALF overall speed. Needs investigation!
            static const uint8_t* __attribute__((noinline)) find_pattern_short_scalar(const uint8_t* const pattern, const uint32_t patLen, const uint8_t* data, uint32_t dataLen) noexcept {

                const uint8_t* const end = data + dataLen;
                if(patLen > sizeof(uint16_t)) {
                    if(patLen == sizeof(uint32_t)) {
                        const uint32_t v = as<uint32_t>(pattern);
                        if constexpr (Arch::XTENSA && Arch::XT_LOOP) {
                            uint32_t tmp = dataLen;
                            asm volatile (
                                "LOOPNEZ %[tmp], end_%=" "\n"
                                    "L32I %[tmp], %[data], 0" "\n"
                                    "BEQ %[tmp], %[v], end_%=" "\n"
                                    "ADDI %[data], %[data], 1" "\n"
                                "end_%=:"
                                : [tmp] "+r" (tmp),
                                  [data] "+r" (data)
                                : [v] "r" (v)
                            );
                        } else {
                            while(data < end && as<uint32_t>(data) != v) {
                                incptr<1>(data);
                            }
                        }
                    }
                    else
                    {
                        // assert patLen == 3
                        const uint32_t v = as<uint32_t>(pattern) << 8;
                        if constexpr (Arch::XTENSA && Arch::XT_LOOP) {
                            uint32_t tmp = dataLen;
                            asm volatile (
                                "LOOPNEZ %[tmp], end_%=" "\n"
                                    "L32I %[tmp], %[data], 0" "\n"
                                    "SLLI %[tmp], %[tmp], 8" "\n"
                                    "BEQ %[tmp], %[v], end_%=" "\n"
                                    "ADDI %[data], %[data], 1" "\n"
                                "end_%=:"
                                : [tmp] "+r" (tmp),
                                  [data] "+r" (data)
                                : [v] "r" (v)
                            );
                        } else {
                            while(data < end && (as<uint32_t>(data) << 8) != v) {
                                incptr<1>(data);
                            }
                        }
                    }
                } else {
                    if(patLen == sizeof(uint16_t)) {
                        const uint32_t v = as<uint16_t>(pattern);
                        if constexpr (Arch::XTENSA && Arch::XT_LOOP) {
                            uint32_t tmp = dataLen;
                            asm volatile (
                                "LOOPNEZ %[tmp], end_%=" "\n"
                                    "L16UI %[tmp], %[data], 0" "\n"
                                    "BEQ %[tmp], %[v], end_%=" "\n"
                                    "ADDI %[data], %[data], 1" "\n"
                                "end_%=:"
                                : [tmp] "+r" (tmp),
                                  [data] "+r" (data)
                                : [v] "r" (v)
                            );
                        } else {
                            while(data < end && as<uint16_t>(data) != v) {
                                incptr<1>(data);
                            }
                        }
                    } else [[unlikely]] {
                        // assert patLen == 1
                        const uint32_t v = as<uint8_t>(pattern);
                        if constexpr (Arch::XTENSA && Arch::XT_LOOP) {
                            uint32_t tmp = dataLen;
                            asm volatile (
                                "LOOPNEZ %[tmp], end_%=" "\n"
                                    "L8UI %[tmp], %[data], 0" "\n"
                                    "BEQ %[tmp], %[v], end_%=" "\n"
                                    "ADDI %[data], %[data], 1" "\n"
                                "end_%=:"
                                : [tmp] "+r" (tmp),
                                  [data] "+r" (data)
                                : [v] "r" (v)
                            );
                        } else {
                            while(data < end && as<uint8_t>(data) != v) {
                                incptr<1>(data);
                            }
                        }
                    }
                }
                return (data < end) ? data : nullptr;
            }

            /**
             * @brief Scalar pattern search for patterns > sizeof(uint32_t) bytes in length.
             *
             * @param pattern
             * @param patLen must be >= sizeof(uint32_t)
             * @param data
             * @param dataLen
             * @return start of the first match found in \p data, or \c nullptr if none found.
             */
            // For some reason, without noinline we get only about HALF overall speed. Needs investigation!
            static const uint8_t* __attribute__((noinline)) find_pattern_long_scalar(const uint8_t* pattern, const uint32_t patLen, const uint8_t* data, uint32_t dataLen) noexcept {
                using T = uint32_t;
                constexpr uint32_t sw = sizeof(T);


                const uint8_t* first = data;
                const uint8_t* last = first + patLen-sw;
                const uint32_t f = as<T>(pattern);
                const uint32_t l = as<T>(pattern + patLen - sw);

                const uint8_t* const end = first + dataLen /*- (sw-1)*/;

                const uint32_t cmpLen = patLen - std::min(patLen,2*sw);

                do {
                    if constexpr (Arch::XTENSA && Arch::XT_LOOP) {
                        uint32_t tmp = end-first;
                        if constexpr (sizeof(T) == sizeof(uint32_t)) {
                            asm (
                                "LOOPNEZ %[tmp], end_%=" "\n"
                                    "L32I %[tmp], %[first], 0" "\n"
                                    "BNE %[tmp], %[f], next_%=" "\n"

                                    "L32I %[tmp], %[last], 0" "\n"
                                    "BEQ %[tmp], %[l], end_%=" "\n"

                                    "next_%=:" "\n"

                                    "ADDI %[first], %[first], 1" "\n"
                                    "ADDI %[last], %[last], 1" "\n"
                                "end_%=:"
                                : [first] "+r" (first),
                                  [last] "+r" (last),
                                  [tmp] "+r" (tmp)
                                : [f] "r" (f),
                                  [l] "r" (l)
                            );
                        } else {
                            asm (
                                "LOOPNEZ %[tmp], end_%=" "\n"
                                    "L%[bits]UI %[tmp], %[first], 0" "\n"
                                    "BNE %[tmp], %[f], next_%=" "\n"

                                    "L%[bits]UI %[tmp], %[last], 0" "\n"
                                    "BEQ %[tmp], %[l], end_%=" "\n"

                                    "next_%=:" "\n"

                                    "ADDI %[first], %[first], 1" "\n"
                                    "ADDI %[last], %[last], 1" "\n"
                                "end_%=:"
                                : [first] "+r" (first),
                                  [last] "+r" (last),
                                  [tmp] "+r" (tmp)
                                : [f] "r" (f),
                                  [l] "r" (l),
                                  [bits] "i" (sizeof(T)*8)
                            );
                        }
                    } else {
                        do {
                            if(f == as<T>(first) && l == as<T>(last)) {
                                break;
                            } else {
                                incptr<1>(first);
                                incptr<1>(last);
                            }
                        } while(first < end);
                        // while ( first < end && (f != *(const uint32_t*)(first) || l != *(const uint32_t*)(last))) {
                        //     incptr<1>(first);
                        //     incptr<1>(last);
                        // } 
                    }

                    if(first < end) {
                        if(cmpLen == 0 || cmp8(first+sw,pattern+sw,cmpLen) >= cmpLen) {
                            return first;
                        } else {
                            incptr<1>(first);
                            incptr<1>(last);
                        }
                    }
                } while (first < end);

                return nullptr;
            }

        public:

            /**
             * @brief Memory compare, like ::cmp(), but only compares single bytes in a loop.
             * This is faster on average if a mismatch within the first few (~4-6 or so) bytes is likely.
             *
             * @param d1
             * @param d2
             * @param len must be >= 1
             * @return uint32_t
             */
            static uint32_t cmp8(const void* d1, const void* d2, const uint32_t len) noexcept {
                const uintptr_t start = (uintptr_t)d1;

                if constexpr (Arch::XTENSA && Arch::XT_LOOP) {

                    // Memory barrier for the compiler
                    asm volatile (""::"m" (*(const uint8_t(*)[len])d1));
                    asm volatile (""::"m" (*(const uint8_t(*)[len])d2));

                    uint32_t tmp1, tmp2;
                    asm volatile (

                        "LOOPNEZ %[cnt], end_%=" "\n"

                            "L8UI %[tmp1], %[d1], 0" "\n"
                            "L8UI %[tmp2], %[d2], 0" "\n"

                            "BNE %[tmp1], %[tmp2], end_%=" "\n"

                            "ADDI %[d1], %[d1], 1" "\n"
                            "ADDI %[d2], %[d2], 1" "\n"

                        "end_%=:"
                        :
                          [tmp1] "=r" (tmp1),
                          [tmp2] "=r" (tmp2),
                          [d1] "+r" (d1),
                          [d2] "+r" (d2)
                        :
                          [cnt] "r" (len)
                    );

                } else {
                    const uint8_t* const end = p<uint8_t>(d1) + len;
                    do {
                        if(as<uint8_t>(d1) != as<uint8_t>(d2)) {
                            break;
                        } else {
                            incptr<1>(d1);
                            incptr<1>(d2);
                        }
                    } while( d1 < end );
                }

                return (uintptr_t)d1-start;
            }

            /**
             * @brief Compare up to \p len bytes from \p d1 and \p d2 and return the length of
             * the common prefix of both memory regions.
             *
             * @param d1 pointer to memory to compare against \p d2
             * @param d2 pointer to memory to compare against \p d1
             * @param len maximum number of bytes to compare
             * @return common prefix length of \p *d1 and \p *d2, i.e. the number of equal bytes
             * from the start of both memory regions; returns \p len if all bytes are equal.
             */
            static uint32_t cmp(const void* d1, const void* d2, const uint32_t len) noexcept {

                const uint8_t* const end = (const uint8_t*)d1 + len;

                if constexpr (Arch::XTENSA && Arch::XT_LOOP) {

                    // Memory barrier for the compiler
                    asm volatile (""::"m" (*(const uint8_t(*)[len])d1));
                    asm volatile (""::"m" (*(const uint8_t(*)[len])d2));

                    {
                        uint32_t tmp1, tmp2;

                        // Start comparing in 4-byte chunks (32bit) until a mismatch is found.
                        asm volatile (

                            "LOOPNEZ %[cnt], end_%=" "\n"

                                "L32I %[tmp1], %[d1], 0" "\n"
                                "L32I %[tmp2], %[d2], 0" "\n"

                                "BNE %[tmp1], %[tmp2], end_%=" "\n"

                                "ADDI %[d1], %[d1], 4" "\n"
                                "ADDI %[d2], %[d2], 4" "\n"

                            "end_%=:"
                            :
                              [tmp1] "=r" (tmp1),
                              [tmp2] "=r" (tmp2),
                              [d1] "+r" (d1),
                              [d2] "+r" (d2)
                            :
                              [cnt] "r" (len/4)
                        );


                    }
                    

                    {
                        // Compare remaining data (0..3 bytes) byte-wise
                        uint32_t tmp1, tmp2;
                        asm volatile (

                                "LOOPNEZ %[cnt], end_%=" "\n"

                                    "L8UI %[tmp1], %[d1], 0" "\n"
                                    "L8UI %[tmp2], %[d2], 0" "\n"

                                    "BNE %[tmp1], %[tmp2], end_%=" "\n"

                                    "ADDI %[d1], %[d1], 1" "\n"
                                    "ADDI %[d2], %[d2], 1" "\n"

                                "end_%=:"
                            :
                              [tmp1] "=r" (tmp1),
                              [tmp2] "=r" (tmp2),
                              [d1] "+r" (d1),
                              [d2] "+r" (d2)
                            :
                              [cnt] "r" (end-(const uint8_t*)d1)
                        );
                    }

                    return (const uint8_t*)d1-(end-len);

                } else {
                    {
                        const void* const end32 = (const uint8_t*)d1 + (len & ~3);
                        while (d1 < end32 && as<uint32_t>(d1) == as<uint32_t>(d2)) {
                            incptr<4>(d1);
                            incptr<4>(d2);
                        }
                    }

                    if(d1 < end) {
                        // Find any common prefix in (d1+0)...(d1+sizeof(uint32_t)-1)
                        const uint32_t sml = subword_match_len(d1,d2);
                        return std::min(len, (uint32_t)((const uint8_t*)d1-(end-len)+sml));
                    } else {
                        return len;
                    }

                }

            }


            /**
             * @brief Scalar, i.e. non-SIMD, pattern search.
             *
             * @param pattern start of pattern to search for
             * @param patLen length of pattern to search for
             * @param data start of data to search
             * @param dataLen length of data to search
             * @return first start of pattern in data, or \c nullptr if not found
             */
            static const uint8_t* find_pattern_scalar(const uint8_t* const pattern, const uint32_t patLen, const uint8_t* data, const uint32_t dataLen) noexcept {
                if(patLen > sizeof(uint32_t)) {
                    return find_pattern_long_scalar(pattern, patLen, data, dataLen);
                } else
                {
                    return find_pattern_short_scalar(pattern,patLen, data, dataLen);
                }
            }



            /**
             * @brief Searches \p data for the first occurence of a \p pattern.
             * On ESP32-S3 targets, this uses SIMD instructions; delegates to ::find_pattern_scalar() on other targets.
             *
             * @param pattern start of pattern to search for
             * @param patLen length of pattern to search for
             * @param data start of data to search
             * @param dataLen length of data to search
             * @return first start of pattern in data, or \c nullptr if not found
             */
            static const uint8_t* find_pattern(const uint8_t* const pattern, const uint32_t patLen, const uint8_t* data, const uint32_t dataLen) noexcept {

                if constexpr (Arch::ESP32S3) {
                    // Memory barrier for the compiler
                    asm volatile (""::"m" (*(const uint8_t(*)[dataLen])data));
                    asm volatile (""::"m" (*(const uint8_t(*)[patLen])pattern));

                    // q7[n] := POS_U8[n]
                    asm volatile (
                        "LD.QR q7, %[bytes]" "\n"
                        :
                        : [bytes] "m" (POS_U8)
                    );

                    // q4[n] := pattern[0]
                    asm volatile (
                        "EE.VLDBC.8 q4, %[first]"
                        :
                        : [first] "r" (pattern)
                    );

                    // q5[n] := pattern[patLen-1]
                    asm volatile (
                        "EE.VLDBC.8 q5, %[last]"
                        :
                        : [last] "r" (pattern + patLen - 1)
                    );


                    // Pointer to potential match on first byte of pattern:
                    const uint8_t* first = data;
                    // Pointer to potential match on last byte of pattern:
                    const uint8_t* last = first + patLen - 1;

                    const uint8_t* const end = first + dataLen;


                    // Using (q1:q0) to load+align 'first' data,
                    // (q3:q2) to load+align 'last' data
                    asm volatile (
                        "EE.VLD.128.IP q0, %[first], 16" "\n"
                        "EE.LD.128.USAR.IP q1, %[first], 16" "\n"

                        "EE.VLD.128.IP q2, %[last], 16" "\n"
                        : [first] "+r" (first),
                          [last] "+r" (last)
                    );

                    const uint8_t* const pat1 = pattern+1;
                    // We won't need to compare the first and the last byte of pattern a second time.
                    const uint32_t cmpLen = patLen - std::min(patLen,(uint32_t)2);

                    // 'first' is ahead by 32 bytes of where we are actually searching.
                    const uint8_t* const flimit = end + 32;

                    do {

                        // Want at least one iteration while there is any data left.
                        uint32_t tmp = ((flimit - first) + 15) / 16;
                        asm volatile (

                            "EE.ZERO.ACCX" "\n"

                            "LOOPNEZ %[tmp], end_%=" "\n"

                                /* Align & compare data from 'first' */
                                "EE.SRC.Q.QUP q1, q0, q1" "\n"

                                    // Pipelining: Load next data from 'last'
                                    "EE.LD.128.USAR.IP q3, %[last], 16" "\n"

                                // q6[n] := first[n] == pattern[0]
                                "EE.VCMP.EQ.S8 q6, q1, q4" "\n"

                                /* Align & compare data from 'last' */
                                "EE.SRC.Q.QUP q3, q2, q3" "\n"
                                // q3[n] := last[n] == pattern[patLen-1]
                                "EE.VCMP.EQ.S8 q3, q3, q5" "\n"

                                // AND both comparison results
                                // q6[n] := q6[n] && q3[n]
                                "EE.ANDQ q6, q6, q3" "\n"

                                // Fold:
                                "EE.VMULAS.S8.ACCX q6, q6" "\n"

                                    // Pipelining: Load next data from 'first'
                                    "EE.LD.128.USAR.IP q1, %[first], 16" "\n"

                                // Get result, exit loop if any matches:
                                "RUR.ACCX_0 %[tmp]" "\n"
                                "BNEZ %[tmp], end_%=" "\n"

                            "end_%=:"
                            : [first] "+r" (first),
                              [last] "+r" (last),
                              [tmp] "+r" (tmp)
                            :
                        );

                        if(tmp) {

                            // The result of the comparison is still in q6.

                            /* Yes, it takes no more than 7 instructions, plus the 'lookup table' from POS_U8 (in q7),
                             * to extract 16 bits from the 16 8-bit boolean results of a vector comparison.
                             *
                             * Seriously. This is the fastest way I could come up with.
                             * The S3's PIE has only ONE horizontal SIMD instruction (VMULAS.ACCX).
                             */
                            asm volatile (

                                "EE.ZERO.ACCX" "\n"

                                // And position factor with comparison mask:
                                // q6[n] := POS_U8[n] & q6[n]
                                "EE.ANDQ q6, q7, q6" "\n"

                                // MAC 1: MAC all (non-masked) elements:
                                "EE.VMULAS.U8.ACCX q6, q6" "\n"

                                // Extract only the even elements from q6, set the rest to 0.
                                "EE.ZERO.Q q3" "\n" // Get zeroes to fill the gaps.
                                "EE.VUNZIP.8 q6, q3" "\n"

                                // MAC the even elements one more time:
                                "EE.VMULAS.U8.ACCX q6, q6" "\n"

                                // Stall the pipeline for 1 cycle.
                                // Then continue:

                                // Extract:
                                "RUR.ACCX_0 %[tmp]" "\n"

                            :
                                [tmp] "=r" (tmp)
                            :
                            );

                            // What we want is now in bits 15...0 of tmp

                            tmp = tmp << 16;
                            const uint8_t* s1 = first-48;

                            do {
                                {
                                    const uint32_t bits = __builtin_clz(tmp) + 1;
                                    s1 += bits;
                                    tmp = tmp << bits;
                                }
                                if(s1 > end) [[unlikely]] {
                                    // Found match begins beyond the end of data.
                                    return nullptr;
                                }
                                if(cmpLen == 0 || cmp8(s1,pat1,cmpLen) >= cmpLen) {
                                    return s1-1;
                                }
                            } while(tmp != 0);
                        }
                    } while(first < flimit);
                    return nullptr;

                } else {
                    return find_pattern_scalar(pattern, patLen, data, dataLen);
                }

            }

            /**
             * @brief Searches \p data for the longest prefix of \p pattern that can be found.
             *
             * @param pattern
             * @param patLen length of the pattern; must be >= 2
             * @param data
             * @param dataLen
             * @return a std::span of the match found in data, or an empty std::span if no prefix was found.
             */
            static byte_span find_longest_match(
                const uint8_t* const pattern,
                const uint32_t patLen,
                const uint8_t* data,
                uint32_t dataLen) noexcept {

                const uint8_t* bestMatch {nullptr};
                uint32_t matchLen {0};

                const uint8_t* const end = data + dataLen;

                {
                    const uint8_t* match;
                    uint32_t searchLen = 2;
                    do {
                        match = find_pattern(pattern, searchLen, data, dataLen);
                        if(match) {
                            bestMatch = match;
                            matchLen = searchLen;
                            if(searchLen < patLen) [[likely]] {
                                // If the current match happens to extend beyond what we searched for,
                                // we'll take that too.
                                matchLen += cmp8(pattern+searchLen, match+searchLen, patLen-searchLen);
                            }
                            dataLen -= match+1-data;
                            data = match+1;

                            searchLen = matchLen+1; // Next match must beat the current one.
                        }
                    } while(match && searchLen <= patLen && data < end);
                }

                return byte_span {bestMatch,matchLen};

            }

    };

} // namespace heatshrink