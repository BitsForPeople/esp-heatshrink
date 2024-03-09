#pragma once
#include <cstdint>
#include <span>
#include <type_traits>
#include <algorithm>

#include "hs_arch.hpp"

namespace heatshrink {

    class Locator {
        private:
        // We multiply (mac) this with itself, then mac the values at all even
        // positions again which makes the even positions 'worth' 2x as much,
        // so we effectively get
        // {2*(128*128),(128*128),2*(64*64),(64*64),...,2*(1*1),(1*1)}, i.e.
        // {(1<<15),(1<<14),(1<<13),...,(1<<1),(1<<0)} as desired.
        alignas(16) static constexpr uint8_t POS_U8[16] {
            128,128,64,64,32,32,16,16,8,8,4,4,2,2,1,1,
        };


    // struct u128 {
    //     uint64_t d[2];
    //     constexpr bool operator ==(const u128& other) const noexcept = default;
    // };

    // // template<typename T>
    // // requires(sizeof(T) <= sizeof(uint64_t))
    // // static constexpr bool _cmp(const uint8_t* &a, const uint8_t* &b, const uint8_t* const end){
    // //     if (a <= (end-sizeof(T))) {
    // //         if(*(const T*)a == *(const T*)b) {
    // //             a += sizeof(T);
    // //             b += sizeof(T);
    // //             return true;
    // //         }
    // //     }
    // //     return false;
    // // } 


    // template<typename T>
    // // requires(sizeof(T) > sizeof(uint64_t))
    // static constexpr bool _cmp(const uint8_t* &a, const uint8_t* &b, const uint8_t* const end) noexcept {
    //     if (a <= (end-sizeof(T))) [[likely]] {
    //         if(*(const T*)a == *(const T*)b) [[likely]] {
    //             a += sizeof(T);
    //             b += sizeof(T);
    //             return true;
    //         }
    //     }
    //     return false;
    // } 

    // template<typename T>
    // static void _c(const uint8_t* &a, const uint8_t* &b) noexcept {
    //     if(*(const T*)a == *(const T*)b) [[likely]] {
    //         a += sizeof(T);
    //         b += sizeof(T);
    //     }
    // }

        static uint32_t subword_match_len(const void* a, const void* b) noexcept {
            if(((const uint16_t*)a)[0] == ((const uint16_t*)b)[0]) {
                if(((const uint8_t*)a)[2] != ((const uint8_t*)b)[2]) {
                    return 2;
                } else {
                    return 3;
                }
            } else {
                if(((const uint8_t*)a)[0] == ((const uint8_t*)b)[0]) {
                    return 1;
                } else {
                    return 0;
                }
            }
            // const uint16_t* pa = (const uint16_t*)a;
            // const uint16_t* pb = (const uint16_t*)b;
            // uint32_t x = (pa[0] == pb[0]) << 1;
            // x += a[x] == b[x];
            // return x;
        }

         template<int32_t INC, typename T>
        static void __attribute__((always_inline)) incptr(T*& ptr) {
            ptr = (T*)(((uintptr_t)ptr) + INC);
        }

            template<typename T>
            static T as(const void* const ptr) {
                return *reinterpret_cast<const T*>(ptr);
            }

            // patLen <= sizeof(uint32_t)!
            static const uint8_t* /*__attribute__((noinline))*/ find_pattern_short_scalar(const uint8_t* const pattern, const uint32_t patLen, const uint8_t* data, uint32_t dataLen) {

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

            // This version limits only the _start_ of a match to be within data, as heatshrink does it.
            template<typename W = uint32_t>
            static const uint8_t* /* __attribute__((noinline)) */ find_pattern_t_scalar(const uint8_t* pattern, const uint32_t patLen, const uint8_t* data, uint32_t dataLen) {
                using T = std::make_unsigned_t<W>;
                constexpr uint32_t sw = sizeof(T);


                const uint8_t* first = data;
                const uint8_t* last = first + patLen-sw;
                const uint32_t f = as<T>(pattern);
                const uint32_t l = as<T>(pattern + patLen - sw);

                const uint32_t skipLen = 1; // Locator::getSkipLen<T>(pattern,patLen);

                const uint8_t* const end = first + dataLen /*- (sw-1)*/;

                const uint32_t cmpLen = patLen - std::min(patLen,2*sw);

                do {
                    if constexpr (Arch::XTENSA && Arch::XT_LOOP) {
                        uint32_t tmp = end-first;
                        if constexpr (sizeof(W) == sizeof(uint32_t)) {
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
                                [bits] "i" (sizeof(W)*8)
                            );            
                        }
                    } else {
                        do {
                            if(f == as<T>(first) && l == as<T>(last)) {
                                break;
                            } else {
                                ++first;
                                ++last;
                            }
                        } while(first < end);
                    }

                    if(first < end) {
                        if(cmpLen == 0 || cmp(first+sw,pattern+sw,cmpLen) >= cmpLen) {
                            return first;
                        } else {
                            first += skipLen;
                            last += skipLen;
                        }
                    }
                } while (first < end);
                return nullptr;
            }

        public:

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
            static uint32_t cmp(const void* d1, const void* d2, const uint32_t len) {

                const uint8_t* const end = (const uint8_t*)d1 + len;

                if constexpr (Arch::XTENSA && Arch::XT_LOOP) {
                    {
                        uint32_t tmp1, tmp2;
               
                        asm volatile (

                            "L32I %[tmp1], %[d1], 0" "\n"
                            "L32I %[tmp2], %[d2], 0" "\n"            

                            "LOOPNEZ %[cnt], end_%=" "\n"

                                "BNE %[tmp1], %[tmp2], end_%=" "\n"

                                "L32I %[tmp1], %[d1], 4" "\n"

                                "ADDI %[d1], %[d1], 4" "\n"

                                "L32I %[tmp2], %[d2], 4" "\n"   

                                "ADDI %[d2], %[d2], 4" "\n"

                            "end_%=:"
                            : [tmp1] "=&r" (tmp1),
                              [tmp2] "=&r" (tmp2),
                              [d1] "+r" (d1),
                              [d2] "+r" (d2)
                            : [cnt] "r" (len/4)
                        );



                        // len = (uintptr_t)end-(uintptr_t)d1;                
                    
                        // if(d1 < end && tmp1 != tmp2) [[likely]] {
                        // //     // Locate the first non-matching byte
                        //     tmp1 = tmp1 ^ tmp2;
                        // //     if(tmp1 != 0) [[likely]] {
                        //         // Find first set:
                        //         tmp1 = tmp1 & -tmp1;
                        //         tmp1 = 31-__builtin_clz(tmp1);
                                
                        //         return ((uintptr_t)d1 + tmp1/8)-((uintptr_t)end-len);
                        // //     }
                        // }
                        // return len;
                    }

                    {
                        uint32_t tmp1, tmp2;
                        asm volatile (

                                "L8UI %[tmp1], %[d1], 0" "\n"
                                "L8UI %[tmp2], %[d2], 0" "\n"            

                                "LOOPNEZ %[cnt], end_%=" "\n"

                                    "BNE %[tmp1], %[tmp2], end_%=" "\n"

                                    "L8UI %[tmp1], %[d1], 1" "\n"

                                    "ADDI %[d1], %[d1], 1" "\n"

                                    "L8UI %[tmp2], %[d2], 1" "\n"   

                                    "ADDI %[d2], %[d2], 1" "\n"

                                "end_%=:"
                            : [tmp1] "=&r" (tmp1),
                              [tmp2] "=&r" (tmp2),
                              [d1] "+r" (d1),
                              [d2] "+r" (d2)
                            : [cnt] "r" (end-(const uint8_t*)d1)
                        );
                    }

                    return (const uint8_t*)d1-(end-len);

                } else {
                    {
                        const uint8_t* const end32 = (const uint8_t*)d1 + (len & ~3);
                        while ((const uint8_t*)d1 < end32 && as<uint32_t>(d1) == as<uint32_t>(d2)) {
                            incptr<4>(d1);
                            incptr<4>(d2);
                        }
                    }

                    if(d1 < end) {
                        const uint32_t sml = subword_match_len(d1,d2);
                        return std::min(len, (const uint8_t*)d1-(end-len)+sml);
                    } else {
                        return len;
                    }                    

                    // while((const uint8_t*)d1 < end && as<uint8_t>(d1) == as<uint8_t>(d2)) {
                    //     incptr<1>(d1);
                    //     incptr<1>(d2);
                    // }

                    // return (const uint8_t*)d1-(end-len);

                }

            }



            static const uint8_t* find_pattern_scalar(const uint8_t* const pattern, const uint32_t patLen, const uint8_t* data, const uint32_t dataLen) {
                if(patLen > sizeof(uint32_t)) {
                    return find_pattern_t_scalar<uint32_t>(pattern, patLen, data, dataLen);
                } else
                {
                    return find_pattern_short_scalar(pattern,patLen, data, dataLen);
                }
            }

            template<std::size_t N, std::size_t M>
            static const uint8_t* find_pattern_scalar(const std::span<const uint8_t,M>& pattern, const std::span<const uint8_t,N>& data) {
                return find_pattern_scalar(pattern.data(), pattern.size_bytes(), data.data(), data.size_bytes());
            }


            // This version limits only the _start_ of a match to be within data, as heatshrink does it.
            static const uint8_t* /*__attribute__((noinline))*/ find_pattern(const uint8_t* const pattern, const uint32_t patLen, const uint8_t* data, const uint32_t dataLen) {

                if constexpr (Arch::ESP32S3) {
                    asm volatile (""::"m" (*(const uint8_t(*)[dataLen])data));
                    asm volatile (""::"m" (*(const uint8_t(*)[patLen])pattern));

                    asm volatile (
                        "LD.QR q7, %[bytes]" "\n"
                        :
                        : [bytes] "m" (POS_U8)
                    );

                    asm volatile (
                        "EE.VLDBC.8 q4, %[first]"
                        :
                        : [first] "r" (pattern)
                    );

                    asm volatile (
                        "EE.VLDBC.8 q5, %[last]"
                        :
                        : [last] "r" (pattern + patLen - 1)
                    );


                    const uint8_t* first = data;
                    const uint8_t* const end = first + dataLen;     
                    const uint8_t* last = first + patLen - 1;


                    asm volatile (
                        "EE.VLD.128.IP q0, %[first], 16" "\n"
                        "EE.LD.128.USAR.IP q1, %[first], 16" "\n"

                        "EE.VLD.128.IP q2, %[last], 16" "\n"  
                        : [first] "+r" (first),
                        [last] "+r" (last)
                    );      

                    const uint8_t* const pat1 = pattern+1;                
                    const uint32_t cmpLen = patLen - std::min(patLen,(uint32_t)2);

                    const uint8_t* const flimit = end + 32;

                    do {

                        uint32_t tmp = ((flimit - first) + 15) / 16;
                        asm volatile (

                            "EE.ZERO.ACCX" "\n"

                            "LOOPNEZ %[tmp], end_%=" "\n"

                                /* Align & compare data from 'first' */
                                "EE.SRC.Q.QUP q1, q0, q1" "\n"    

                                    // Pipelining: Load next data from 'last'
                                    "EE.LD.128.USAR.IP q3, %[last], 16" "\n"

                                "EE.VCMP.EQ.S8 q6, q1, q4" "\n"

                                /* Align & compare data from 'last' */
                                "EE.SRC.Q.QUP q3, q2, q3" "\n"
                                "EE.VCMP.EQ.S8 q3, q3, q5" "\n"

                                // AND both comparison results
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
                             */
                            asm volatile (

                                "EE.ZERO.ACCX" "\n"

                                // And position factor with comparison mask:
                                "EE.ANDQ q6, q7, q6" "\n"

                                // MAC 1: MAC all (non-masked) elements:
                                "EE.VMULAS.U8.ACCX q6, q6" "\n"

                                // Extract only the even elements from q6, set the rest to 0.
                                "EE.ZERO.Q q3" "\n" // Get zeroes to fill the gaps.                
                                "EE.VUNZIP.8 q6, q3" "\n"

                                // MAC the even elements one more time:
                                "EE.VMULAS.U8.ACCX q6, q6" "\n"

                                // Extract:
                                "RUR.ACCX_0 %[tmp]" "\n"

                            : [tmp] "=r" (tmp)
                            : 
                            );                


                            tmp = tmp << 16;
                            const uint8_t* s1 = first-48;

                            do {
                                {
                                    const uint32_t bits = __builtin_clz(tmp) + 1;
                                    s1 += bits;
                                    tmp = tmp << (bits);
                                }
                                if(s1 > end) [[unlikely]] {
                                    // Found match begins beyond the end of data.
                                    return nullptr;
                                }
                                if(cmpLen == 0 || cmp(s1,pat1,cmpLen) >= cmpLen) {
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

            template<std::size_t N, std::size_t M>
            static const uint8_t* find_pattern(const std::span<const uint8_t,M>& pattern, const std::span<const uint8_t,N>& data) {
                return find_pattern(pattern.data(), pattern.size_bytes(), data.data(), data.size_bytes());
            }

            static std::span<const uint8_t> /* __attribute__((noinline)) */ find_longest_match(
                const uint8_t* const pattern,
                const uint32_t patLen,
                const uint8_t* data,
                uint32_t dataLen) {

                const uint8_t* bestMatch {nullptr};
                uint32_t matchLen {0};

                {
                    const uint8_t* match;
                    uint32_t searchLen = 2;
                    do {
                        match = find_pattern(pattern, searchLen, data, dataLen);
                        if(match) {
                            bestMatch = match;
                            matchLen = searchLen;
                            if(searchLen < patLen) [[likely]] {
                                matchLen += cmp(pattern+searchLen, match+searchLen, patLen-searchLen);
                            }
                            dataLen -= match+1-data;
                            data = match+1;
                            
                            searchLen = matchLen+1; // Next match must beat the current one.
                        }
                    } while(match && searchLen <= patLen && searchLen <= dataLen);
                }

                return std::span<const uint8_t> {bestMatch,matchLen};

            }

            template<std::size_t N, std::size_t M>
            static const std::span<const uint8_t> find_longest_match(const std::span<const uint8_t,N>& pattern, const std::span<const uint8_t,M>& data) {
                return find_longest_match(
                    pattern.data(),
                    pattern.size_bytes(),
                    data.data(),
                    data.size_bytes()
                );
            }

    };

} // namespace heatshrink