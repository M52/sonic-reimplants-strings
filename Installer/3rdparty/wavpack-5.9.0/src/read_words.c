////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2013 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// read_words.c

// This module provides entropy word decoding functions using
// a variation on the Rice method.  This was introduced in version 3.93
// because it allows splitting the data into a "lossy" stream and a
// "correction" stream in a very efficient manner and is therefore ideal
// for the "hybrid" mode.  For 4.0, the efficiency of this method was
// significantly improved by moving away from the normal Rice restriction of
// using powers of two for the modulus divisions and now the method can be
// used for both hybrid and pure lossless encoding.

// Samples are divided by median probabilities at 5/7 (71.43%), 10/49 (20.41%),
// and 20/343 (5.83%). Each zone has 3.5 times fewer samples than the
// previous. Using standard Rice coding on this data would result in 1.4
// bits per sample average (not counting sign bit). However, there is a
// very simple encoding that is over 99% efficient with this data and
// results in about 1.22 bits per sample.

#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "wavpack_local.h"

#if defined (HAVE___BUILTIN_CTZ) || defined (_WIN64)
#define USE_CTZ_OPTIMIZATION    // use ctz intrinsic (or Windows equivalent) to count trailing ones
#elif defined(__WATCOMC__) && defined(__386__)
#define USE_CTZ_OPTIMIZATION
extern __inline uint32_t _bsf_watcom (uint32_t);
#pragma aux _bsf_watcom = \
  "bsf eax, eax" \
  parm [eax] nomemory \
  value [eax] \
  modify exact [eax] nomemory;
#else
#define USE_NEXT8_OPTIMIZATION  // optimization using a table to count trailing ones
#endif

#define USE_BITMASK_TABLES      // use tables instead of shifting for certain masking operations

///////////////////////////// local table storage ////////////////////////////

#ifdef USE_NEXT8_OPTIMIZATION
static const char ones_count_table [] = {
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,5,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,6,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,5,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,7,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,5,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,6,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,5,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,8
};
#endif

///////////////////////////// executable code ////////////////////////////////

static uint32_t __inline read_code (Bitstream *bs, uint32_t maxcode);

// Read the next word from the bitstream "wvbits" and return the value. This
// function can be used for hybrid or lossless streams, but since an
// optimized version is available for lossless this function would normally
// be used for hybrid only. If a hybrid lossless stream is being read then
// the "correction" offset is written at the specified pointer. A return value
// of WORD_EOF indicates that the end of the bitstream was reached (all 1s) or
// some other error occurred.

int32_t FASTCALL get_word (WavpackStream *wps, int chan, int32_t *correction)
{
    struct entropy_data *c = wps->w.c + chan;
    uint32_t ones_count, low, mid, high;
    int32_t value;
    int sign;

    if (!wps->wvbits.ptr)
        return WORD_EOF;

    if (correction)
        *correction = 0;

    if (!(wps->w.c [0].median [0] & ~1) && !wps->w.holding_zero && !wps->w.holding_one && !(wps->w.c [1].median [0] & ~1)) {
        uint32_t mask;
        int cbits;

        if (wps->w.zeros_acc) {
            if (--wps->w.zeros_acc) {
                c->slow_level -= (c->slow_level + SLO) >> SLS;
                return 0;
            }
        }
        else {
            for (cbits = 0; cbits < 33 && getbit (&wps->wvbits); ++cbits);

            if (cbits == 33)
                return WORD_EOF;

            if (cbits < 2)
                wps->w.zeros_acc = cbits;
            else {
                for (mask = 1, wps->w.zeros_acc = 0; --cbits; mask <<= 1)
                    if (getbit (&wps->wvbits))
                        wps->w.zeros_acc |= mask;

                wps->w.zeros_acc |= mask;
            }

            if (wps->w.zeros_acc) {
                c->slow_level -= (c->slow_level + SLO) >> SLS;
                CLEARA (wps->w.c [0].median);
                CLEARA (wps->w.c [1].median);
                return 0;
            }
        }
    }

    if (wps->w.holding_zero)
        ones_count = wps->w.holding_zero = 0;
    else {
#ifdef USE_CTZ_OPTIMIZATION
        while (wps->wvbits.bc < LIMIT_ONES) {
            if (++(wps->wvbits.ptr) == wps->wvbits.end)
                wps->wvbits.wrap (&wps->wvbits);

            wps->wvbits.sr |= *(wps->wvbits.ptr) << wps->wvbits.bc;
            wps->wvbits.bc += sizeof (*(wps->wvbits.ptr)) * 8;
        }

#ifdef _MSC_VER
        { unsigned long res; _BitScanForward (&res, (unsigned long)~wps->wvbits.sr); ones_count = (uint32_t) res; }
#elif defined(__WATCOMC__) && defined(__386__)
        ones_count = _bsf_watcom (~wps->wvbits.sr);
#else
        ones_count = __builtin_ctz (~wps->wvbits.sr);
#endif

        if (ones_count >= LIMIT_ONES) {
            wps->wvbits.bc -= ones_count;
            wps->wvbits.sr >>= ones_count;

            for (; ones_count < (LIMIT_ONES + 1) && getbit (&wps->wvbits); ++ones_count);

            if (ones_count == (LIMIT_ONES + 1))
                return WORD_EOF;

            if (ones_count == LIMIT_ONES) {
                uint32_t mask;
                int cbits;

                for (cbits = 0; cbits < 33 && getbit (&wps->wvbits); ++cbits);

                if (cbits == 33)
                    return WORD_EOF;

                if (cbits < 2)
                    ones_count = cbits;
                else {
                    for (mask = 1, ones_count = 0; --cbits; mask <<= 1)
                        if (getbit (&wps->wvbits))
                            ones_count |= mask;

                    ones_count |= mask;
                }

                ones_count += LIMIT_ONES;
            }
        }
        else {
            wps->wvbits.bc -= ones_count + 1;
            wps->wvbits.sr >>= ones_count + 1;
        }
#elif defined (USE_NEXT8_OPTIMIZATION)
        int next8;

        if (wps->wvbits.bc < 8) {
            if (++(wps->wvbits.ptr) == wps->wvbits.end)
                wps->wvbits.wrap (&wps->wvbits);

            next8 = (wps->wvbits.sr |= *(wps->wvbits.ptr) << wps->wvbits.bc) & 0xff;
            wps->wvbits.bc += sizeof (*(wps->wvbits.ptr)) * 8;
        }
        else
            next8 = wps->wvbits.sr & 0xff;

        if (next8 == 0xff) {
            wps->wvbits.bc -= 8;
            wps->wvbits.sr >>= 8;

            for (ones_count = 8; ones_count < (LIMIT_ONES + 1) && getbit (&wps->wvbits); ++ones_count);

            if (ones_count == (LIMIT_ONES + 1))
                return WORD_EOF;

            if (ones_count == LIMIT_ONES) {
                uint32_t mask;
                int cbits;

                for (cbits = 0; cbits < 33 && getbit (&wps->wvbits); ++cbits);

                if (cbits == 33)
                    return WORD_EOF;

                if (cbits < 2)
                    ones_count = cbits;
                else {
                    for (mask = 1, ones_count = 0; --cbits; mask <<= 1)
                        if (getbit (&wps->wvbits))
                            ones_count |= mask;

                    ones_count |= mask;
                }

                ones_count += LIMIT_ONES;
            }
        }
        else {
            wps->wvbits.bc -= (ones_count = ones_count_table [next8]) + 1;
            wps->wvbits.sr >>= ones_count + 1;
        }
#else
        for (ones_count = 0; ones_count < (LIMIT_ONES + 1) && getbit (&wps->wvbits); ++ones_count);

        if (ones_count >= LIMIT_ONES) {
            uint32_t mask;
            int cbits;

            if (ones_count == (LIMIT_ONES + 1))
                return WORD_EOF;

            for (cbits = 0; cbits < 33 && getbit (&wps->wvbits); ++cbits);

            if (cbits == 33)
                return WORD_EOF;

            if (cbits < 2)
                ones_count = cbits;
            else {
                for (mask = 1, ones_count = 0; --cbits; mask <<= 1)
                    if (getbit (&wps->wvbits))
                        ones_count |= mask;

                ones_count |= mask;
            }

            ones_count += LIMIT_ONES;
        }
#endif

        if (wps->w.holding_one) {
            wps->w.holding_one = ones_count & 1;
            ones_count = (ones_count >> 1) + 1;
        }
        else {
            wps->w.holding_one = ones_count & 1;
            ones_count >>= 1;
        }

        wps->w.holding_zero = ~wps->w.holding_one & 1;
    }

    if ((wps->wphdr.flags & HYBRID_FLAG) && !chan)
        update_error_limit (wps);

    if (ones_count == 0) {
        low = 0;
        high = GET_MED (0) - 1;
        DEC_MED0 ();
    }
    else {
        low = GET_MED (0);
        INC_MED0 ();

        if (ones_count == 1) {
            high = low + GET_MED (1) - 1;
            DEC_MED1 ();
        }
        else {
            low += GET_MED (1);
            INC_MED1 ();

            if (ones_count == 2) {
                high = low + GET_MED (2) - 1;
                DEC_MED2 ();
            }
            else {
                low += (ones_count - 2) * GET_MED (2);
                high = low + GET_MED (2) - 1;
                INC_MED2 ();
            }
        }
    }

    low &= 0x7fffffff;
    high &= 0x7fffffff;

    if (low > high)         // make sure high and low make sense
        high = low;

    mid = (high + low + 1) >> 1;

    if (!c->error_limit)
        mid = read_code (&wps->wvbits, high - low) + low;
    else while (high - low > c->error_limit) {
        if (getbit (&wps->wvbits))
            mid = (high + (low = mid) + 1) >> 1;
        else
            mid = ((high = mid - 1) + low + 1) >> 1;
    }

    sign = getbit (&wps->wvbits);

    if (bs_is_open (&wps->wvcbits) && c->error_limit) {
        value = read_code (&wps->wvcbits, high - low) + low;

        if (correction)
            *correction = sign ? (mid - value) : (value - mid);
    }

    if (wps->wphdr.flags & HYBRID_BITRATE) {
        c->slow_level -= (c->slow_level + SLO) >> SLS;
        c->slow_level += wp_log2 (mid);
    }

    return sign ? ~mid : mid;
}

// This is an optimized version of get_word() that is used for lossless only
// (error_limit == 0). Also, rather than obtaining a single sample, it can be
// used to obtain an entire buffer of either mono or stereo samples.
//
// FastSampler: this is the stock loop, unchanged except that it now counts
// values rather than frames, starts at csamples_start, and returns the value
// count. The fast decoder below hands the tail of a buffer to it; see there.

static int32_t get_words_lossless_ref (WavpackStream *wps, int32_t *buffer, int32_t nsamples, int32_t csamples_start)
{
    struct entropy_data *c = wps->w.c;
    uint32_t ones_count, low, high;
    Bitstream *bs = &wps->wvbits;
    int32_t csamples;
#ifdef USE_NEXT8_OPTIMIZATION
    int32_t next8;
#endif

    for (csamples = csamples_start; csamples < nsamples; ++csamples) {
        if (!(wps->wphdr.flags & MONO_DATA))
            c = wps->w.c + (csamples & 1);

        if (wps->w.holding_zero) {
            wps->w.holding_zero = 0;
            low = read_code (bs, GET_MED (0) - 1);
            DEC_MED0 ();
            buffer [csamples] = (getbit (bs)) ? ~low : low;

            if (++csamples == nsamples)
                break;

            if (!(wps->wphdr.flags & MONO_DATA))
                c = wps->w.c + (csamples & 1);
        }

        if (wps->w.c [0].median [0] < 2 && !wps->w.holding_one && wps->w.c [1].median [0] < 2) {
            uint32_t mask;
            int cbits;

            if (wps->w.zeros_acc) {
                if (--wps->w.zeros_acc) {
                    buffer [csamples] = 0;
                    continue;
                }
            }
            else {
                for (cbits = 0; cbits < 33 && getbit (bs); ++cbits);

                if (cbits == 33)
                    break;

                if (cbits < 2)
                    wps->w.zeros_acc = cbits;
                else {
                    for (mask = 1, wps->w.zeros_acc = 0; --cbits; mask <<= 1)
                        if (getbit (bs))
                            wps->w.zeros_acc |= mask;

                    wps->w.zeros_acc |= mask;
                }

                if (wps->w.zeros_acc) {
                    CLEARA (wps->w.c [0].median);
                    CLEARA (wps->w.c [1].median);
                    buffer [csamples] = 0;
                    continue;
                }
            }
        }

#ifdef USE_CTZ_OPTIMIZATION
        while (bs->bc < LIMIT_ONES) {
            if (++(bs->ptr) == bs->end)
                bs->wrap (bs);

            bs->sr |= *(bs->ptr) << bs->bc;
            bs->bc += sizeof (*(bs->ptr)) * 8;
        }

#ifdef _MSC_VER
        { unsigned long res; _BitScanForward (&res, (unsigned long)~wps->wvbits.sr); ones_count = (uint32_t) res; }
#elif defined(__WATCOMC__) && defined(__386__)
        ones_count = _bsf_watcom (~wps->wvbits.sr);
#else
        ones_count = __builtin_ctz (~wps->wvbits.sr);
#endif

        if (ones_count >= LIMIT_ONES) {
            bs->bc -= ones_count;
            bs->sr >>= ones_count;

            for (; ones_count < (LIMIT_ONES + 1) && getbit (bs); ++ones_count);

            if (ones_count == (LIMIT_ONES + 1))
                break;

            if (ones_count == LIMIT_ONES) {
                uint32_t mask;
                int cbits;

                for (cbits = 0; cbits < 33 && getbit (bs); ++cbits);

                if (cbits == 33)
                    break;

                if (cbits < 2)
                    ones_count = cbits;
                else {
                    for (mask = 1, ones_count = 0; --cbits; mask <<= 1)
                        if (getbit (bs))
                            ones_count |= mask;

                    ones_count |= mask;
                }

                ones_count += LIMIT_ONES;
            }
        }
        else {
            bs->bc -= ones_count + 1;
            bs->sr >>= ones_count + 1;
        }
#elif defined (USE_NEXT8_OPTIMIZATION)
        if (bs->bc < 8) {
            if (++(bs->ptr) == bs->end)
                bs->wrap (bs);

            next8 = (bs->sr |= *(bs->ptr) << bs->bc) & 0xff;
            bs->bc += sizeof (*(bs->ptr)) * 8;
        }
        else
            next8 = bs->sr & 0xff;

        if (next8 == 0xff) {
            bs->bc -= 8;
            bs->sr >>= 8;

            for (ones_count = 8; ones_count < (LIMIT_ONES + 1) && getbit (bs); ++ones_count);

            if (ones_count == (LIMIT_ONES + 1))
                break;

            if (ones_count == LIMIT_ONES) {
                uint32_t mask;
                int cbits;

                for (cbits = 0; cbits < 33 && getbit (bs); ++cbits);

                if (cbits == 33)
                    break;

                if (cbits < 2)
                    ones_count = cbits;
                else {
                    for (mask = 1, ones_count = 0; --cbits; mask <<= 1)
                        if (getbit (bs))
                            ones_count |= mask;

                    ones_count |= mask;
                }

                ones_count += LIMIT_ONES;
            }
        }
        else {
            bs->bc -= (ones_count = ones_count_table [next8]) + 1;
            bs->sr >>= ones_count + 1;
        }
#else
        for (ones_count = 0; ones_count < (LIMIT_ONES + 1) && getbit (bs); ++ones_count);

        if (ones_count >= LIMIT_ONES) {
            uint32_t mask;
            int cbits;

            if (ones_count == (LIMIT_ONES + 1))
                break;

            for (cbits = 0; cbits < 33 && getbit (bs); ++cbits);

            if (cbits == 33)
                break;

            if (cbits < 2)
                ones_count = cbits;
            else {
                for (mask = 1, ones_count = 0; --cbits; mask <<= 1)
                    if (getbit (bs))
                        ones_count |= mask;

                ones_count |= mask;
            }

            ones_count += LIMIT_ONES;
        }
#endif

        low = wps->w.holding_one;
        wps->w.holding_one = ones_count & 1;
        wps->w.holding_zero = ~ones_count & 1;
        ones_count = (ones_count >> 1) + low;

        if (ones_count == 0) {
            low = 0;
            high = GET_MED (0) - 1;
            DEC_MED0 ();
        }
        else {
            low = GET_MED (0);
            INC_MED0 ();

            if (ones_count == 1) {
                high = low + GET_MED (1) - 1;
                DEC_MED1 ();
            }
            else {
                low += GET_MED (1);
                INC_MED1 ();

                if (ones_count == 2) {
                    high = low + GET_MED (2) - 1;
                    DEC_MED2 ();
                }
                else {
                    low += (ones_count - 2) * GET_MED (2);
                    high = low + GET_MED (2) - 1;
                    INC_MED2 ();
                }
            }
        }

        low += read_code (bs, high - low);
        buffer [csamples] = (getbit (bs)) ? ~low : low;
    }

    return csamples;
}

// Read a single unsigned value from the specified bitstream with a value
// from 0 to maxcode. If there are exactly a power of two number of possible
// codes then this will read a fixed number of bits; otherwise it reads the
// minimum number of bits and then determines whether another bit is needed
// to define the code.

static uint32_t __inline read_code (Bitstream *bs, uint32_t maxcode)
{
    unsigned long local_sr;
    uint32_t extras, code;
    int bitcount;

    if (maxcode < 2)
        return maxcode ? getbit (bs) : 0;

    bitcount = count_bits (maxcode);
#ifdef USE_BITMASK_TABLES
    extras = bitset [bitcount] - maxcode - 1;
#else
    extras = (1 << bitcount) - maxcode - 1;
#endif

    local_sr = bs->sr;

    while (bs->bc < bitcount) {
        if (++(bs->ptr) == bs->end)
            bs->wrap (bs);

        local_sr |= (long)*(bs->ptr) << bs->bc;
        bs->bc += sizeof (*(bs->ptr)) * 8;
    }

#ifdef USE_BITMASK_TABLES
    if ((code = local_sr & bitmask [bitcount - 1]) >= extras)
#else
    if ((code = local_sr & ((1 << (bitcount - 1)) - 1)) >= extras)
#endif
        code = (code << 1) - extras + ((local_sr >> (bitcount - 1)) & 1);
    else
        bitcount--;

    if (sizeof (local_sr) < 8 && bs->bc > sizeof (local_sr) * 8) {
        bs->bc -= bitcount;
        bs->sr = *(bs->ptr) >> (sizeof (*(bs->ptr)) * 8 - bs->bc);
    }
    else {
        bs->bc -= bitcount;
        bs->sr = local_sr >> bitcount;
    }

    return code;
}

#ifdef BITSTREAM_SHORTS
// =============================================================================
// FastSampler variant of get_words_lossless, 2026-09-10.
//
// Same algorithm, same output. Two things change. The decoder state (the bit
// reader, the six medians, the holding flags and the zero run) lives in
// locals for the whole call instead of behind the WavpackStream and Bitstream
// pointers: the stock loop stores buffer[csamples] every sample, the compiler
// cannot prove that store does not alias bs->sr, bs->bc, bs->ptr or the
// medians, so every one of them is reloaded and stored back around every bit
// read. And the bitstream is read through a 64-bit window refilled in bulk,
// so the unary prefix, the residual code and the sign are shifts on one
// register rather than a load-modify-store per bit.
//
// The stock loop is kept above as get_words_lossless_ref and takes over
// wherever the fast loop does not want to be exact by itself: within
// FAST_SLACK_SHORTS of the end of the block, so the fast loop never needs the
// wrap callback and never reads past the buffer; and on a run of 17 or more
// ones at a code boundary, which is the end-of-stream marker (or corruption)
// and whose handling in the stock code depends on how many bits it happens to
// hold. Neither case occurs inside a valid stream decoded for the number of
// samples it holds. The hand-over is at a sample boundary with the reader
// written back in the stock representation, so the two loops agree bit for
// bit.
// =============================================================================

#define FAST_REFILL_TO 48       // refill while this many bits or fewer are held; one more short always fits
#define FAST_SLACK_SHORTS 32    // hand over to the stock loop this close to the end of the block

// Unlike TZCNT, BitScanForward64 also works on pre-BMI x64 CPUs.
// A full 64-bit all-ones window makes ~window zero; both implementations
// explicitly return 64 rather than invoke an undefined builtin operation.
static __inline int fast_tzcnt64 (uint64_t value)
{
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned long index;
    return _BitScanForward64 (&index, value) ? (int) index : 64;
#elif defined(__GNUC__) || defined(__clang__)
    return value ? __builtin_ctzll (value) : 64;
#else
    int count = 0;
    if (!value) return 64;
    while (!(value & 1)) { value >>= 1; ++count; }
    return count;
#endif
}

// =============================================================================
// The window: bit 0 is the next bit to read, wbc bits are valid, and every bit
// above wbc is zero, so the trailing-zero count of ~window never runs past
// the valid bits. Refilling adds whole shorts at the top.
// =============================================================================
#define FAST_REFILL() do { \
    while (wbc <= FAST_REFILL_TO) { \
        window |= (uint64_t) *++wptr << wbc; \
        wbc += 16; \
    } } while (0)

#define FAST_CONSUME(n) do { window >>= (n); wbc -= (n); } while (0)

#define FAST_GETBIT(result) do { (result) = (uint32_t) window & 1; FAST_CONSUME (1); } while (0)

// =============================================================================
// read_code on the window: the same arithmetic as the stock read_code, with
// the tables replaced by shifts. Needs count_bits (maxcode) + 1 bits held.
// =============================================================================
#define FAST_READ_CODE(result, maxcode) do { \
    uint32_t mc_ = (maxcode); \
    if (mc_ < 2) { \
        if (mc_) FAST_GETBIT (result); \
        else (result) = 0; \
    } \
    else { \
        int bitcount_ = count_bits (mc_); \
        uint32_t extras_ = (uint32_t) ((1ull << bitcount_) - mc_ - 1); \
        uint32_t code_ = (uint32_t) window & ((1u << (bitcount_ - 1)) - 1); \
        if (code_ >= extras_) \
            code_ = (code_ << 1) - extras_ + ((uint32_t) (window >> (bitcount_ - 1)) & 1); \
        else \
            bitcount_--; \
        FAST_CONSUME (bitcount_); \
        (result) = code_; \
    } } while (0)

#define FAST_GET_MED(m) (((m) >> 4) + 1)
#define FAST_INC_MED0(m) ((m) += (((m) + DIV0) / DIV0) * 5)
#define FAST_DEC_MED0(m) ((m) -= (((m) + (DIV0-2)) / DIV0) * 2)
#define FAST_INC_MED1(m) ((m) += (((m) + DIV1) / DIV1) * 5)
#define FAST_DEC_MED1(m) ((m) -= (((m) + (DIV1-2)) / DIV1) * 2)
#define FAST_INC_MED2(m) ((m) += (((m) + DIV2) / DIV2) * 5)
#define FAST_DEC_MED2(m) ((m) -= (((m) + (DIV2-2)) / DIV2) * 2)

static int32_t get_words_lossless_fast1 (WavpackStream *wps, int32_t * __restrict buffer, int32_t nsamples)
{
    Bitstream *bs = &wps->wvbits;
    const int stereo = !(wps->wphdr.flags & MONO_DATA);
    const int32_t chmask = stereo ? 1 : 0;
    int32_t nvalues, csamples;
    int handover = 0;

    uint32_t med [2][3];
    uint32_t holding_one = wps->w.holding_one;
    uint32_t zeros_acc = wps->w.zeros_acc;
    int holding_zero = wps->w.holding_zero;

    uint64_t window, snap_window;
    int wbc, snap_wbc;
    const uint16_t *wptr, *snap_wptr;
    const uint16_t *wend;

    if (nsamples <= 0) return 0;

    if (!bs->ptr) {
        memset (buffer, 0, stereo ? nsamples * 8 : nsamples * 4);
        return nsamples;
    }

    nvalues = stereo ? nsamples * 2 : nsamples;


    med [0][0] = wps->w.c [0].median [0];
    med [0][1] = wps->w.c [0].median [1];
    med [0][2] = wps->w.c [0].median [2];
    med [1][0] = wps->w.c [1].median [0];
    med [1][1] = wps->w.c [1].median [1];
    med [1][2] = wps->w.c [1].median [2];

    window = bs->sr;
    wbc = bs->bc;
    wptr = bs->ptr;
    wend = bs->end;

    for (csamples = 0; csamples < nvalues; ++csamples) {
        uint32_t *m;
        uint32_t ones_count, low, high, bit;

        // =============================================================================
        // Everything below reads at most a few shorts; hand over before that
        // could reach the end of the block. Checked before any bit of this
        // sample is consumed, so the stock loop starts exactly here.
        // =============================================================================
        if (wend - wptr <= FAST_SLACK_SHORTS) {
            handover = 1;
            break;
        }

        FAST_REFILL ();
        m = med [csamples & chmask];

        if (holding_zero) {

            holding_zero = 0;
            FAST_READ_CODE (low, FAST_GET_MED (m [0]) - 1);
            FAST_DEC_MED0 (m [0]);
            FAST_GETBIT (bit);
            buffer [csamples] = bit ? ~low : low;

            if (++csamples == nvalues)
                break;

            m = med [csamples & chmask];
            FAST_REFILL ();
        }

        if (med [0][0] < 2 && !holding_one && med [1][0] < 2) {
            uint64_t saved_window = window;
            const uint16_t *saved_wptr = wptr;
            uint32_t saved_zeros_acc = zeros_acc;
            int saved_wbc = wbc;

            if (zeros_acc) {
                if (--zeros_acc) {

                    buffer [csamples] = 0;
                    continue;
                }
            }
            else {
                int cbits = fast_tzcnt64 (~window);

                // =============================================================================
                // 33 ones is the stock loop's end-of-stream exit. Nothing has
                // been consumed yet, so it re-reads the same bits and exits
                // the same way.
                // =============================================================================
                if (cbits >= 33) {
                    handover = 1;
                    break;
                }

                FAST_CONSUME (cbits + 1);

                if (cbits < 2)
                    zeros_acc = cbits;
                else {
                    uint32_t nb = (uint32_t) cbits - 1;
                    FAST_REFILL ();
                    zeros_acc = ((uint32_t) window & ((1u << nb) - 1)) | (1u << nb);
                    FAST_CONSUME (nb);
                }

                if (zeros_acc) {

                    med [0][0] = med [0][1] = med [0][2] = 0;
                    med [1][0] = med [1][1] = med [1][2] = 0;
                    buffer [csamples] = 0;
                    continue;
                }

                FAST_REFILL ();
            }

            // =============================================================================
            // The zero-run step has changed zeros_acc and maybe the bit
            // position, and the value goes on with a unary count. Every
            // hand-over below must start the stock loop at the start of the
            // value, so hand over here, with the step undone, whenever that
            // count could lead to one.
            // =============================================================================
            if (fast_tzcnt64 (~window) >= LIMIT_ONES) {
                window = saved_window;
                wptr = saved_wptr;
                zeros_acc = saved_zeros_acc;
                wbc = saved_wbc;
                handover = 1;
                break;
            }
        }

        FAST_REFILL ();
        snap_window = window;
        snap_wbc = wbc;
        snap_wptr = wptr;

        ones_count = fast_tzcnt64 (~window);

        if (ones_count >= LIMIT_ONES) {
            int cbits;

            // =============================================================================
            // Seventeen or more ones is the end-of-stream marker, and what the
            // stock loop then does depends on its own refill history. Let it.
            // =============================================================================
            if (ones_count > LIMIT_ONES) {
                handover = 1;
                break;
            }

            // =============================================================================
            // Exactly sixteen ones and a zero: the escape, followed by the
            // count in an Elias-gamma style code.
            // =============================================================================

            FAST_CONSUME (LIMIT_ONES + 1);
            FAST_REFILL ();
            cbits = fast_tzcnt64 (~window);

            if (cbits >= 33) {
                window = snap_window;
                wbc = snap_wbc;
                wptr = snap_wptr;
                handover = 1;
                break;
            }

            FAST_CONSUME (cbits + 1);

            if (cbits < 2)
                ones_count = (uint32_t) cbits;
            else {
                uint32_t nb = (uint32_t) cbits - 1;
                FAST_REFILL ();
                ones_count = ((uint32_t) window & ((1u << nb) - 1)) | (1u << nb);
                FAST_CONSUME (nb);
            }

            ones_count += LIMIT_ONES;
        }
        else
            FAST_CONSUME (ones_count + 1);

        low = holding_one;
        holding_one = ones_count & 1;
        holding_zero = ~ones_count & 1;
        ones_count = (ones_count >> 1) + low;


        if (ones_count == 0) {
            low = 0;
            high = FAST_GET_MED (m [0]) - 1;
            FAST_DEC_MED0 (m [0]);
        }
        else {
            low = FAST_GET_MED (m [0]);
            FAST_INC_MED0 (m [0]);

            if (ones_count == 1) {
                high = low + FAST_GET_MED (m [1]) - 1;
                FAST_DEC_MED1 (m [1]);
            }
            else {
                low += FAST_GET_MED (m [1]);
                FAST_INC_MED1 (m [1]);

                if (ones_count == 2) {
                    high = low + FAST_GET_MED (m [2]) - 1;
                    FAST_DEC_MED2 (m [2]);
                }
                else {
                    low += (ones_count - 2) * FAST_GET_MED (m [2]);
                    high = low + FAST_GET_MED (m [2]) - 1;
                    FAST_INC_MED2 (m [2]);
                }
            }
        }

        FAST_REFILL ();
        FAST_READ_CODE (bit, high - low);
        low += bit;
        FAST_GETBIT (bit);
        buffer [csamples] = bit ? ~low : low;
    }

    // =============================================================================
    // Write the reader back in the stock representation: whole unread shorts
    // are given back to the pointer, the remaining bits (fewer than sixteen)
    // stay in sr with zeros above them, so the bit position is unchanged.
    // =============================================================================
    {
        int k = wbc >> 4;
        bs->ptr = (uint16_t *) (wptr - k);
        bs->bc = wbc & 15;
        bs->sr = (uint32_t) window & ((1u << bs->bc) - 1);
    }

    wps->w.holding_one = holding_one;
    wps->w.holding_zero = holding_zero;
    wps->w.zeros_acc = zeros_acc;
    wps->w.c [0].median [0] = med [0][0];
    wps->w.c [0].median [1] = med [0][1];
    wps->w.c [0].median [2] = med [0][2];
    wps->w.c [1].median [0] = med [1][0];
    wps->w.c [1].median [1] = med [1][1];
    wps->w.c [1].median [2] = med [1][2];

    if (handover) {

        csamples = get_words_lossless_ref (wps, buffer, nvalues, csamples);
    }

    return stereo ? csamples / 2 : csamples;
}

// =============================================================================
// Decode one value into buffer[IDX] on the channel whose medians are M0, M1,
// M2. Rare exits jump to handover with the index of the value not decoded,
// with the state as it was at the start of that value; the zero-run step
// changes it before the unary count, so a hand-over after it undoes it.
// LBL is a unique label name for this expansion's "value done" point.
// =============================================================================
#define F4_VALUE(M0, M1, M2, IDX, LBL) do { \
    uint32_t ones_count_, low_, high_, bit_; \
    if (wend - wptr <= FAST_SLACK_SHORTS) { handover_at = (IDX); goto handover; } \
    FAST_REFILL (); \
    if (holding_zero) { \
        holding_zero = 0; \
        FAST_READ_CODE (low_, FAST_GET_MED (M0) - 1); \
        FAST_DEC_MED0 (M0); \
        FAST_GETBIT (bit_); \
        buffer [IDX] = bit_ ? ~low_ : low_; \
        goto LBL; \
    } \
    if (m00 < 2 && !holding_one && m10 < 2) { \
        uint64_t saved_window_ = window; \
        const uint16_t *saved_wptr_ = wptr; \
        uint32_t saved_zeros_acc_ = zeros_acc; \
        int saved_wbc_ = wbc; \
        if (zeros_acc) { \
            if (--zeros_acc) { buffer [IDX] = 0; goto LBL; } \
        } \
        else { \
            int cbits_ = fast_tzcnt64 (~window); \
            if (cbits_ >= 33) { handover_at = (IDX); goto handover; } \
            FAST_CONSUME (cbits_ + 1); \
            if (cbits_ < 2) \
                zeros_acc = (uint32_t) cbits_; \
            else { \
                uint32_t nb_ = (uint32_t) cbits_ - 1; \
                FAST_REFILL (); \
                zeros_acc = ((uint32_t) window & ((1u << nb_) - 1)) | (1u << nb_); \
                FAST_CONSUME (nb_); \
            } \
            if (zeros_acc) { \
                m00 = m01 = m02 = m10 = m11 = m12 = 0; \
                buffer [IDX] = 0; \
                goto LBL; \
            } \
            FAST_REFILL (); \
        } \
        if (fast_tzcnt64 (~window) >= LIMIT_ONES) { \
            window = saved_window_; wptr = saved_wptr_; \
            zeros_acc = saved_zeros_acc_; wbc = saved_wbc_; \
            handover_at = (IDX); goto handover; \
        } \
    } \
    snap_window = window; snap_wbc = wbc; snap_wptr = wptr; \
    ones_count_ = (uint32_t) fast_tzcnt64 (~window); \
    if (ones_count_ >= LIMIT_ONES) { \
        int cbits_; \
        if (ones_count_ > LIMIT_ONES) { handover_at = (IDX); goto handover; } \
        FAST_CONSUME (LIMIT_ONES + 1); \
        FAST_REFILL (); \
        cbits_ = fast_tzcnt64 (~window); \
        if (cbits_ >= 33) { \
            window = snap_window; wbc = snap_wbc; wptr = snap_wptr; \
            handover_at = (IDX); goto handover; \
        } \
        FAST_CONSUME (cbits_ + 1); \
        if (cbits_ < 2) \
            ones_count_ = (uint32_t) cbits_; \
        else { \
            uint32_t nb_ = (uint32_t) cbits_ - 1; \
            FAST_REFILL (); \
            ones_count_ = ((uint32_t) window & ((1u << nb_) - 1)) | (1u << nb_); \
            FAST_CONSUME (nb_); \
        } \
        ones_count_ += LIMIT_ONES; \
        FAST_REFILL (); \
    } \
    else \
        FAST_CONSUME (ones_count_ + 1); \
    low_ = holding_one; \
    holding_one = ones_count_ & 1; \
    holding_zero = ~ones_count_ & 1; \
    ones_count_ = (ones_count_ >> 1) + low_; \
    if (ones_count_ == 0) { \
        low_ = 0; \
        high_ = FAST_GET_MED (M0) - 1; \
        FAST_DEC_MED0 (M0); \
    } \
    else { \
        low_ = FAST_GET_MED (M0); \
        FAST_INC_MED0 (M0); \
        if (ones_count_ == 1) { \
            high_ = low_ + FAST_GET_MED (M1) - 1; \
            FAST_DEC_MED1 (M1); \
        } \
        else { \
            low_ += FAST_GET_MED (M1); \
            FAST_INC_MED1 (M1); \
            if (ones_count_ == 2) { \
                high_ = low_ + FAST_GET_MED (M2) - 1; \
                FAST_DEC_MED2 (M2); \
            } \
            else { \
                low_ += (ones_count_ - 2) * FAST_GET_MED (M2); \
                high_ = low_ + FAST_GET_MED (M2) - 1; \
                FAST_INC_MED2 (M2); \
            } \
        } \
    } \
    FAST_READ_CODE (bit_, high_ - low_); \
    low_ += bit_; \
    FAST_GETBIT (bit_); \
    buffer [IDX] = bit_ ? ~low_ : low_; \
    LBL: ; \
    } while (0)

static int32_t get_words_lossless_c (WavpackStream *wps, int32_t * __restrict buffer, int32_t nsamples)
{
    Bitstream *bs = &wps->wvbits;
    int32_t nvalues, csamples, handover_at;

    uint32_t m00, m01, m02, m10, m11, m12;
    uint32_t holding_one, zeros_acc;
    int holding_zero;

    uint64_t window, snap_window;
    int wbc, snap_wbc;
    const uint16_t *wptr, *snap_wptr;
    const uint16_t *wend;

    if ((wps->wphdr.flags & MONO_DATA) || !bs->ptr || nsamples <= 0)
        return get_words_lossless_fast1 (wps, buffer, nsamples);

    nvalues = nsamples * 2;

    m00 = wps->w.c [0].median [0];
    m01 = wps->w.c [0].median [1];
    m02 = wps->w.c [0].median [2];
    m10 = wps->w.c [1].median [0];
    m11 = wps->w.c [1].median [1];
    m12 = wps->w.c [1].median [2];
    holding_one = wps->w.holding_one;
    holding_zero = wps->w.holding_zero;
    zeros_acc = wps->w.zeros_acc;

    window = bs->sr;
    wbc = bs->bc;
    wptr = bs->ptr;
    wend = bs->end;

    for (csamples = 0; csamples < nvalues; csamples += 2) {
        F4_VALUE (m00, m01, m02, csamples, done_left);
        F4_VALUE (m10, m11, m12, csamples + 1, done_right);
    }
    handover_at = nvalues;

handover:
    {
        int k = wbc >> 4;
        bs->ptr = (uint16_t *) (wptr - k);
        bs->bc = wbc & 15;
        bs->sr = (uint32_t) window & ((1u << bs->bc) - 1);
    }

    wps->w.holding_one = holding_one;
    wps->w.holding_zero = holding_zero;
    wps->w.zeros_acc = zeros_acc;
    wps->w.c [0].median [0] = m00;
    wps->w.c [0].median [1] = m01;
    wps->w.c [0].median [2] = m02;
    wps->w.c [1].median [0] = m10;
    wps->w.c [1].median [1] = m11;
    wps->w.c [1].median [2] = m12;

    csamples = handover_at;
    if (csamples < nvalues)
        csamples = get_words_lossless_ref (wps, buffer, nvalues, csamples);

    return csamples / 2;
}

#ifdef FASTSAMPLER_WAVPACK_ASM
#include <stddef.h>

typedef struct {
    uint64_t window;            //   0
    const uint8_t *bptr;        //   8
    const uint8_t *bend;        //  16
    uint32_t wbc;               //  24
    uint32_t holding_one;       //  28
    uint32_t holding_zero;      //  32
    uint32_t zeros_acc;         //  36
    uint32_t med [2][3];        //  40
    uint64_t chflip;            //  64  12 for stereo, 0 for mono
    uint64_t ch;                //  72  current channel byte offset, 0 or 12
    uint64_t snap_window;       //  80
    const uint8_t *snap_bptr;   //  88
    uint32_t snap_wbc;          //  96
    uint32_t pad;               // 100
    int32_t *buflim;            // 104  last output slot a token may start at
    int32_t *bufbase;           // 112
} WpWordsAsmState;

// =============================================================================
// The kernel hard-codes these offsets; keep the two files in step.
// =============================================================================
typedef char f5_check_window  [offsetof (WpWordsAsmState, window)      ==   0 ? 1 : -1];
typedef char f5_check_bptr    [offsetof (WpWordsAsmState, bptr)        ==   8 ? 1 : -1];
typedef char f5_check_bend    [offsetof (WpWordsAsmState, bend)        ==  16 ? 1 : -1];
typedef char f5_check_wbc     [offsetof (WpWordsAsmState, wbc)         ==  24 ? 1 : -1];
typedef char f5_check_h1      [offsetof (WpWordsAsmState, holding_one) ==  28 ? 1 : -1];
typedef char f5_check_hz      [offsetof (WpWordsAsmState, holding_zero)==  32 ? 1 : -1];
typedef char f5_check_zeros   [offsetof (WpWordsAsmState, zeros_acc)   ==  36 ? 1 : -1];
typedef char f5_check_med     [offsetof (WpWordsAsmState, med)         ==  40 ? 1 : -1];
typedef char f5_check_chflip  [offsetof (WpWordsAsmState, chflip)      ==  64 ? 1 : -1];
typedef char f5_check_ch      [offsetof (WpWordsAsmState, ch)          ==  72 ? 1 : -1];
typedef char f5_check_snapw   [offsetof (WpWordsAsmState, snap_window) ==  80 ? 1 : -1];
typedef char f5_check_snapp   [offsetof (WpWordsAsmState, snap_bptr)   ==  88 ? 1 : -1];
typedef char f5_check_snapb   [offsetof (WpWordsAsmState, snap_wbc)    ==  96 ? 1 : -1];
typedef char f5_check_buflim  [offsetof (WpWordsAsmState, buflim)      == 104 ? 1 : -1];
typedef char f5_check_bufbase [offsetof (WpWordsAsmState, bufbase)     == 112 ? 1 : -1];

extern int32_t fs_wp_words_avx2 (WpWordsAsmState *st, int32_t *buffer);

static int32_t get_words_lossless_asm (WavpackStream *wps, int32_t *buffer, int32_t nsamples)
{
    Bitstream *bs = &wps->wvbits;
    const int stereo = !(wps->wphdr.flags & MONO_DATA);
    WpWordsAsmState st;
    int32_t nvalues, csamples;

    if (nsamples <= 0) return 0;

    if (!bs->ptr) {
        memset (buffer, 0, stereo ? nsamples * 8 : nsamples * 4);
        return nsamples;
    }

    if (nsamples <= 0)
        return 0;

    nvalues = stereo ? nsamples * 2 : nsamples;

    // The speculative token writes two slots. Keep tiny requests entirely
    // in C, including mono one-sample calls, without forming buffer - 1.
    if (nvalues < 2)
        return get_words_lossless_c (wps, buffer, nsamples);

    st.window = bs->sr;
    st.bptr = (const uint8_t *) (bs->ptr + 1);
    st.bend = (const uint8_t *) bs->end;
    st.wbc = (uint32_t) bs->bc;
    st.holding_one = wps->w.holding_one;
    st.holding_zero = (uint32_t) wps->w.holding_zero;
    st.zeros_acc = wps->w.zeros_acc;
    st.med [0][0] = wps->w.c [0].median [0];
    st.med [0][1] = wps->w.c [0].median [1];
    st.med [0][2] = wps->w.c [0].median [2];
    st.med [1][0] = wps->w.c [1].median [0];
    st.med [1][1] = wps->w.c [1].median [1];
    st.med [1][2] = wps->w.c [1].median [2];
    st.chflip = stereo ? 12 : 0;
    st.ch = 0;
    st.buflim = (int32_t *) ((char *) buffer + ((int64_t) nvalues - 2) * 4);
    st.bufbase = buffer;

    csamples = fs_wp_words_avx2 (&st, buffer);

    // =============================================================================
    // Back to the stock representation: the bit position is bytes loaded
    // times eight minus bits held; the stock keeps a short pointer to the
    // last loaded short and the bits left of it in sr, zeros above.
    // =============================================================================
    {
        const uint16_t *buf = bs->buf;
        int64_t pos = ((int64_t) (st.bptr - (const uint8_t *) buf)) * 8 - (int64_t) st.wbc;
        int64_t k = pos >> 4;
        int r = (int) (pos & 15);

        if (r) {
            bs->ptr = (uint16_t *) buf + k;
            bs->bc = 16 - r;
        }
        else {
            bs->ptr = (uint16_t *) buf + k - 1;
            bs->bc = 0;
        }
        bs->sr = (uint32_t) st.window & ((1u << bs->bc) - 1);
    }

    wps->w.holding_one = st.holding_one;
    wps->w.holding_zero = (int) st.holding_zero;
    wps->w.zeros_acc = st.zeros_acc;
    wps->w.c [0].median [0] = st.med [0][0];
    wps->w.c [0].median [1] = st.med [0][1];
    wps->w.c [0].median [2] = st.med [0][2];
    wps->w.c [1].median [0] = st.med [1][0];
    wps->w.c [1].median [1] = st.med [1][1];
    wps->w.c [1].median [2] = st.med [1][2];

    if (csamples < nvalues)
        csamples = get_words_lossless_ref (wps, buffer, nvalues, csamples);

    return stereo ? csamples / 2 : csamples;
}

// Cache CPUID once with atomic publication: independent decode workers can
// enter here concurrently. No AVX instructions or vector state are used by
// the kernel despite x86inc's historical _avx2 suffix.
static int words_use_asm (void)
{
    static volatile long cached;
    long choice = _InterlockedCompareExchange (&cached, 0, 0);
    if (!choice) {
        int cpu[4], bmi = 0, lzcnt = 0;
        __cpuid (cpu, 0);
        if ((unsigned) cpu[0] >= 7) {
            __cpuidex (cpu, 7, 0);
            bmi = (cpu[1] & ((1 << 3) | (1 << 8))) == ((1 << 3) | (1 << 8));
        }
        __cpuid (cpu, (int) 0x80000000u);
        if ((unsigned) cpu[0] >= 0x80000001u) {
            __cpuid (cpu, (int) 0x80000001u);
            lzcnt = (cpu[2] & (1 << 5)) != 0;
        }
        choice = bmi && lzcnt ? 2 : 1;
        _InterlockedCompareExchange (&cached, choice, 0);
    }
    return choice == 2;
}
#endif // FASTSAMPLER_WAVPACK_ASM
#endif // BITSTREAM_SHORTS

int32_t get_words_lossless (WavpackStream *wps, int32_t *buffer, int32_t nsamples)
{
    if (nsamples <= 0) return 0;
#ifdef BITSTREAM_SHORTS
#ifdef FASTSAMPLER_WAVPACK_ASM
    if (words_use_asm ())
        return get_words_lossless_asm (wps, buffer, nsamples);
#endif
    return get_words_lossless_c (wps, buffer, nsamples);
#else
    // The optimized window uses 16-bit little-endian bitstream units.
    if (!wps->wvbits.ptr) {
        memset (buffer, 0, (size_t) nsamples * ((wps->wphdr.flags & MONO_DATA) ? 4 : 8));
        return nsamples;
    }
    {
        int stereo = !(wps->wphdr.flags & MONO_DATA);
        int32_t values = get_words_lossless_ref (wps, buffer, stereo ? nsamples * 2 : nsamples, 0);
        return stereo ? values / 2 : values;
    }
#endif
}
