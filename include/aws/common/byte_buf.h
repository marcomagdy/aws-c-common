#ifndef AWS_STRING_H
#define AWS_STRING_H
/*
* Copyright 2010-2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License").
* You may not use this file except in compliance with the License.
* A copy of the License is located at
*
*  http://aws.amazon.com/apache2.0
*
* or in the "license" file accompanying this file. This file is distributed
* on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
* express or implied. See the License for the specific language governing
* permissions and limitations under the License.
*/

#include <aws/common/common.h>
#include <aws/common/array_list.h>
#include <string.h>
#include <stdint.h>

/**
 * Represents a length-delimited binary string or buffer.
 */
struct aws_byte_buf {
    uint8_t *buffer;
    size_t len;
};

static inline struct aws_byte_buf aws_byte_buf_from_literal(const char *literal) {
    return (struct aws_byte_buf){ .buffer = (uint8_t *)literal, .len = strlen(literal)};
}

static inline struct aws_byte_buf aws_byte_buf_from_c_str(const char *c_str, size_t len) {
    return (struct aws_byte_buf){.buffer = (uint8_t *)c_str, .len = len};
}

static inline struct aws_byte_buf aws_byte_buf_from_array(const uint8_t c_str[], size_t len) {
    return (struct aws_byte_buf){.buffer = (uint8_t *)&c_str[0], .len = len};
}

#ifdef __cplusplus
extern "C" {
#endif

/**
 * No copies, no string allocations. Fills in output with a list of aws_byte_buf instances where buffer is
 * an offset into the input_str and len is the length of that string in the original buffer.
 *
 * Edge case rules are as follows:
 * if the string begins with split_on, an empty string will be the first entry in output
 * if the input string has two adjacent split_on tokens, an empty string will be inserted into the output.
 * if the input string ends with split_on, it will be ignored.
 *
 * It is the user's responsibility to properly initialize output. Recommended number of preallocated elements from output
 * is your most likely guess for the upper bound of the number of elements resulting from the split.
 *
 * It is the user's responsibility to make sure the input buffer stays in memory long enough to use the results.
 */
AWS_COMMON_API int aws_string_split_on_char(struct aws_byte_buf *input_str, char split_on,
                                            struct aws_array_list *output);

/**
 * No copies, no string allocations. Fills in output with a list of aws_byte_buf instances where buffer is
 * an offset into the input_str and len is the length of that string in the original buffer.
 *
 * Edge case rules are as follows:
 * if the string begins with split_on, an empty string will be the first entry in output
 * if the input string has two adjacent split_on tokens, an empty string will be inserted into the output.
 * if the input string ends with split_on, it will be ignored.
 *
 * It is the user's responsibility to properly initialize output. Recommended number of preallocated elements from output
 * is your most likely guess for the upper bound of the number of elements resulting from the split.
 *
 * It is the user's responsibility to make sure the input buffer stays in memory long enough to use the results.
 *
 * We assume split_on is a literal and will call strlen() on it.
 */
AWS_COMMON_API int aws_string_split_on_str(struct aws_byte_buf *input_str, char *split_on,
                                            struct aws_array_list *output);
#ifdef __cplusplus
}
#endif


/**
 * Represents a movable pointer within a larger binary string or buffer.
 */
struct aws_byte_cursor {
    uint8_t *ptr;
    size_t len;
};

static inline struct aws_byte_cursor aws_byte_cursor_from_buf(const struct aws_byte_buf *buf) {
    return (struct aws_byte_cursor){.ptr = buf->buffer, .len = buf->len};
}

/**
 * If index >= bound, bound > (SIZE_MAX / 2), or index > (SIZE_MAX / 2), returns 0.
 * Otherwise, returns index.  This function is designed to return a value
 * within [0, bound) even under CPU speculation conditions, and is intended to
 * be used for SPECTRE mitigation purposes.
 */
static inline size_t aws_nospec_index(size_t index, size_t bound) {
    /*
     * SPECTRE mitigation - we compute a mask that will be zero if len < 0
     * or len >= buf->len, and all-ones otherwise, and AND it into the index.
     * It is critical that we avoid any branches in this logic.
     */

    /*
     * Hide the index value from the optimizer. This helps ensure that all this
     * logic doesn't get eliminated.
     */
#if defined(__GNUC__) || defined(__clang__)
     __asm__ __volatile__("" : "+r" (index));
#endif
#if defined(_MSVC_LANG)
     /*
      * MSVC doesn't have a good way for us to blind the optimizer, and doesn't
      * even have inline asm on x64. Some experimentation indicates that this
      * hack seems to confuse it sufficiently for our needs.
      */
     *((volatile uint8_t *)&index) += 0;
#endif

    /*
     * If len > (SIZE_MAX / 2), then we can end up with len - buf->len being
     * positive simply because the sign bit got inverted away. So we also check
     * that the sign bit isn't set from the start.
     *
     * We also check that bound <= (SIZE_MAX / 2) to catch cases where the buffer
     * is _already_ out of bounds.
     */
    size_t negative_mask = index | bound;
    size_t toobig_mask = bound - index - (uintptr_t)1;
    size_t combined_mask = negative_mask | toobig_mask;

    /*
     * combined_mask needs to have its sign bit OFF for us to be in range.
     * We'd like to expand this to a mask we can AND into our index, so flip
     * that bit (and everything else), shift it over so it's the only bit in the
     * ones position, and multiply across the entire register.
     *
     * First, extract the (inverse) top bit and move it to the lowest bit.
     * Because there's no standard SIZE_BIT in C99, we'll divide by a mask with
     * just the top bit set instead.
     */

    combined_mask = (~combined_mask) / (SIZE_MAX - (SIZE_MAX >> 1));

    /*
     * Now multiply it to replicate it across all bits.
     *
     * Note that GCC is smart enough to optimize the divide-and-multiply into
     * an arithmetic right shift operation on x86.
     */
    combined_mask = combined_mask * SIZE_MAX;

    return index & combined_mask;
}

/**
 * Tests if the given aws_byte_cursor has at least len bytes remaining. If so,
 * *buf is advanced by len bytes (incrementing ->ptr and decrementing ->len),
 * and an aws_byte_cursor referring to the first len bytes of the original *buf is
 * returned. Otherwise, an aws_byte_cursor with ->ptr = NULL, ->len = 0 is returned.
 *
 * Note that if len is above (SIZE_MAX / 2), this function will also treat it as a buffer
 * overflow, and return NULL without changing *buf.
 */
static inline struct aws_byte_cursor aws_byte_cursor_advance(struct aws_byte_cursor *cursor, size_t len) {
    struct aws_byte_cursor rv;
    if (cursor->len > (SIZE_MAX >> 1) || len > (SIZE_MAX >> 1) || len > cursor->len) {
        rv.ptr = NULL;
        rv.len = 0;
    } else {
        rv.ptr = cursor->ptr;
        rv.len = len;

        cursor->ptr += len;
        cursor->len -= len;
    }

    return rv;
}

/**
 * Behaves identically to aws_byte_cursor_advance, but avoids speculative
 * execution potentially reading out-of-bounds pointers (by returning an
 * empty ptr in such speculated paths).
 *
 * This should generally be done when using an untrusted or
 * data-dependent value for 'len', to avoid speculating into a path where
 * cursor->ptr points outside the true ptr length.
 */

static inline struct aws_byte_cursor aws_byte_cursor_advance_nospec(struct aws_byte_cursor *cursor, size_t len) {
    struct aws_byte_cursor rv;

    if (len <= cursor->len && len <= (SIZE_MAX >> 1) && cursor->len <= (SIZE_MAX >> 1)) {
        /*
         * Pass the length through aws_nospec_index. We do this after the branch, as otherwise
         * we'd treat an out-of-bounds read as a zero-length read.
         */
        len = aws_nospec_index(len, cursor->len + 1);

        rv.ptr = cursor->ptr;
        rv.len = len;

        cursor->ptr += len;
        cursor->len -= len;
    } else {
        rv.ptr = NULL;
        rv.len = 0;
    }

    return rv;
}


#endif /* AWS_STRING_H */
