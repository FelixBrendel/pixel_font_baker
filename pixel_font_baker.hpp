/*
 * BSD 2-Clause License
 *
 * Copyright (c) 2022, Felix Brendel
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <ftb/types.hpp>
#include <ftb/macros.hpp>
#include <ftb/io.hpp>

// NOTE(Felix): Pixel_Font* can be casted to Waveshare's sFONT*
struct Pixel_Font {
    u8* table;
    u16 char_px_width;
    u16 char_px_height;

    // Additional (redundant) info, not in Waveshare's sFONT:
    u32 bytes_per_line;
    u32 bytes_per_glyph;
};

enum struct Pixel_Font_Baker_Error {
    SUCCESS,
    MALLOC_FAILED,
    FONT_FILE_COULD_NOT_BE_OPENED,
    BDF_DID_NOT_SPECIFY_FONTBOUNDINGBOX,
    BDF_DID_NOT_SPECIFY_FONTBOUNDINGBOX_CORRECTLY,
    BDF_DID_NOT_SPECIFY_CODEPOINT_CORRECTLY,
    BDF_ERROR_PARSING_CHARACTER_BYTES,
    STB_TRUETYPE_FAILED,
};

Pixel_Font_Baker_Error create_pixel_font_from_bdf(const char* font_path,
                                                  u32 unicode_cp_start, u32 unicode_cp_end,
                                                  Pixel_Font* out_font);

Pixel_Font_Baker_Error create_pixel_font_from_ttf(const char* font_path, u16 char_height_in_px,
                                                  u32 unicode_cp_start, u32 unicode_cp_end,
                                                  u8 gray_threashold, u8 supersample, Pixel_Font* out_font);

void destroy_pixel_font(Pixel_Font* out_font);

#ifdef PIXEL_FONT_BAKER_IMPL
#include <stdio.h>
#include "stb_truetype.h"

#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef max
#define max(x, y) ((x) > (y) ? (x) : (y))
#endif

Pixel_Font_Baker_Error create_pixel_font_from_bdf(const char* font_path,
                                                  u32 unicode_cp_start, u32 unicode_cp_end,
                                                  Pixel_Font* out_font)
{
    String file_content = read_entire_file(font_path);
    if (!file_content)
        return Pixel_Font_Baker_Error::FONT_FILE_COULD_NOT_BE_OPENED;
    defer { file_content.free(); };

    String cursor = file_content;

    auto advance_cursor = [&](s32 amount = 1) -> void {
        cursor.data   += amount;
        cursor.length -= amount;
    };

    auto is_on_newline = [&]() -> bool {
        return (*cursor.data == '\r'
                || *cursor.data == '\n');
    };

    auto is_on_whitespace = [&]() -> bool {
        return (*cursor.data == ' '
                || *cursor.data == '\t'
                || is_on_newline());
    };

    auto eat_whitespace = [&]() -> void {
        while (is_on_whitespace())
            advance_cursor();
    };

    auto eat_until_next_line = [&]() -> void {
        // find newline
        while (!is_on_newline() && cursor.length > 0)
            advance_cursor();
        // then eat all the whitespace
        eat_whitespace();
    };

    auto eat_until_line_that_starts_with_and_skip = [&](const char* to_search, u32 to_search_length)
        -> void
    {
        eat_whitespace();
        while (strncmp(cursor.data, to_search, to_search_length) != 0 && cursor.length > 0) {
            eat_until_next_line();
        }

        advance_cursor((s32)min(cursor.length, to_search_length)); // min prevents running out of the string
    };


    s32 font_size_x, font_size_y, start_x, start_y;
    // NOTE(Felix): Find the font dimensions (FONTBOUNDINGBOX)
    {
        const char to_search[] = "FONTBOUNDINGBOX ";
        u32 to_search_length   = sizeof(to_search) - 1; // sizeof includes null terminator
        eat_until_line_that_starts_with_and_skip(to_search, to_search_length);

        if (cursor.length == 0)
            return Pixel_Font_Baker_Error::BDF_DID_NOT_SPECIFY_FONTBOUNDINGBOX;

        // we are on the FONTBOUNDINGBOX line.
        {
            s32 filled_vars =
                sscanf(cursor.data, "%d %d %d %d",
                   &font_size_x, &font_size_y, &start_x, &start_y);

            if (filled_vars != 4) {
                return Pixel_Font_Baker_Error::BDF_DID_NOT_SPECIFY_FONTBOUNDINGBOX_CORRECTLY;
            }
        }
    }

    // NOTE(Felix): allocate the needed memory
    {
        out_font->char_px_width   = font_size_x;
        out_font->char_px_height  = font_size_y;
        out_font->bytes_per_line  = (font_size_x / 8) + (font_size_x % 8 != 0);
        out_font->bytes_per_glyph = font_size_y * out_font->bytes_per_line;

        u32 total_byte_size =
            out_font->bytes_per_glyph * (unicode_cp_end - unicode_cp_start + 1); // both inclusive so +1

        out_font->table = (u8*)malloc(total_byte_size);
        if (!out_font->table)
            return Pixel_Font_Baker_Error::MALLOC_FAILED;

        // NOTE(Felix): Initialize with checker board (if bytes_per_line is 1 or
        //   2, otherwise its just stripes)
        if (out_font->bytes_per_line == 1)
            memset(out_font->table, 0b0101'0101'1010'1010, total_byte_size);
        else if (out_font->bytes_per_line == 2)
            memset(out_font->table, 0b0101'1010'0101'1010, total_byte_size);
        else
            memset(out_font->table, 0b0101'0101'0101'0101, total_byte_size);

    }

    // NOTE(Felix): read all relevant bitmaps until end of file
    while (cursor.length > 0) {
        { // NOTE(Felix): Jump to the codepoint
            const char to_search[] = "ENCODING ";
            u32 to_search_length   = sizeof(to_search) - 1; // sizeof includes null terminator
            eat_until_line_that_starts_with_and_skip(to_search, to_search_length);

            if (cursor.length <= 0)
                break;
        }

        s32 code_point = 0;
        s32 filled_vars =
            sscanf(cursor.data, "%d", &code_point);

        if (filled_vars != 1) {
            return Pixel_Font_Baker_Error::BDF_DID_NOT_SPECIFY_CODEPOINT_CORRECTLY;
        }

        if (code_point < (s32)unicode_cp_start || code_point > (s32)unicode_cp_end)
            continue;

        byte* table_entry =
            out_font->table + (out_font->bytes_per_glyph *
                               (code_point - unicode_cp_start));

        { // NOTE(Felix): Jump to the numbers
            const char to_search[] = "BITMAP";
            u32 to_search_length   = sizeof(to_search) - 1; // sizeof includes null terminator
            eat_until_line_that_starts_with_and_skip(to_search, to_search_length);
            eat_whitespace();
        }

        // NOTE(Felix): Safety check that we won't run out of the string when
        //   reading all the bytes
        {
            u64 needed_bytes =
                out_font->char_px_height*(2*(out_font->bytes_per_line)+1);
            // +1 for newline,
            // *2 for 2 chars for 1 byte in hex
            if (needed_bytes >= cursor.length)
                return Pixel_Font_Baker_Error::BDF_ERROR_PARSING_CHARACTER_BYTES;
        }

        // NOTE(Felix): We now have to read <out_font->char_px_height> many
        //   lines, made of <out_font->bytes_per_line> bytes each. Each byte is
        //   represented by 2 hex-digits
        {
            for (u32 row = 0; row < out_font->char_px_height; ++row) {
                for (u32 col = 0; col < out_font->bytes_per_line; ++col) {
                    u32 value;
                    s32 filled = sscanf(cursor.data, "%2x", &value);
                    if (filled != 1) {
                        return Pixel_Font_Baker_Error::BDF_ERROR_PARSING_CHARACTER_BYTES;
                    }
                    *table_entry = (u8)value;
                    ++table_entry;
                    advance_cursor(2);
                }
                eat_whitespace();
            }
        }
    }

    return Pixel_Font_Baker_Error::SUCCESS;
}

Pixel_Font_Baker_Error create_pixel_font_from_ttf(const char* font_path, u16 char_height_in_px,
                                                  u32 unicode_cp_start, u32 unicode_cp_end,
                                                  u8 gray_threashold, u8 supersample, Pixel_Font* out_font)
{
    stbtt_fontinfo font;
    {
        //
        //  Reading font file
        //
        u8* ttf_buffer = (u8*)malloc(1<<25);
        if (!ttf_buffer) return Pixel_Font_Baker_Error::MALLOC_FAILED;

        FILE* font_file = fopen(font_path, "rb");
        if (!font_file) return Pixel_Font_Baker_Error::FONT_FILE_COULD_NOT_BE_OPENED;

        fread(ttf_buffer, 1, 1<<25, font_file);
        fclose(font_file);

        s32 success =
            stbtt_InitFont(&font, ttf_buffer, stbtt_GetFontOffsetForIndex(ttf_buffer,0));

        if (success == 0) return Pixel_Font_Baker_Error::STB_TRUETYPE_FAILED;
    }

    // internally calculate with higher resolution
    char_height_in_px *= supersample;

    //
    //  Preparing one-char-bitmap
    //
    u8* bitmap;
    s32 unicode_cp_size  = unicode_cp_end - unicode_cp_start + 1;

    s32 char_width_in_px;
    f32 font_scale  = stbtt_ScaleForPixelHeight(&font, char_height_in_px);
    stbtt_GetCodepointHMetrics(&font, 'W', &char_width_in_px, nullptr);
    char_width_in_px = (s32)ceil(char_width_in_px*font_scale);

    // get number of bytes per pixel line per char
    s32 bytes_per_line =
        ((char_width_in_px/supersample) % 8 == 0) ?
        ((char_width_in_px/supersample) / 8)      :
        (((char_width_in_px/supersample) / 8) + 1);


    uint8_t* bitmap_memory = (uint8_t*)malloc(char_height_in_px*char_width_in_px);
    if (!bitmap_memory)
        return Pixel_Font_Baker_Error::MALLOC_FAILED;

    //
    //  Preparing pixel-font-array
    //

    s32 ascend;
    s32 descend;
    s32 line_gap;
    stbtt_GetFontVMetrics(&font, &ascend, &descend, &line_gap);
    ascend = (int)(ascend*font_scale +.5f);
    descend = char_height_in_px - ascend;

    s32 total_byte_size = unicode_cp_size * (char_height_in_px/supersample) * bytes_per_line;
    u8* font_data = (u8*)malloc(total_byte_size);
    if (!font_data)
        return Pixel_Font_Baker_Error::MALLOC_FAILED;

    memset((void*)font_data, 0, total_byte_size);

    //
    //  Fill the pixel-font
    //

    out_font->char_px_width   = char_width_in_px  / supersample;
    out_font->char_px_height  = char_height_in_px / supersample;
    out_font->table           = font_data;
    out_font->bytes_per_line  = bytes_per_line;
    out_font->bytes_per_glyph = bytes_per_line * (char_height_in_px / supersample);

    log_debug("width:           %i", out_font->char_px_width);
    log_debug("height:          %i", out_font->char_px_height);
    log_debug("bytes per line:  %i", out_font->bytes_per_line);
    log_debug("bytes per glyph: %i", out_font->bytes_per_glyph);

    for (u32 cp = unicode_cp_start; cp <= unicode_cp_end; ++cp) {

        s32 bmp_width_in_px;
        s32 bmp_height_in_px;
        s32 x_offset;
        s32 y_offset;

        bitmap = stbtt_GetCodepointBitmap(&font, 0, font_scale, cp,
                                          &bmp_width_in_px, &bmp_height_in_px,
                                          &x_offset, &y_offset);


        {
            memset(bitmap_memory, 0, char_width_in_px*char_height_in_px);
            bmp_width_in_px  = char_width_in_px;
            bmp_height_in_px = char_height_in_px;

            f32 sub_x, sub_y;
            stbtt_MakeCodepointBitmapSubpixelPrefilter(&font, bitmap_memory,
                                                       char_width_in_px, char_height_in_px,
                                                       char_width_in_px, // NOTE(Felix): stride
                                                       font_scale, font_scale, // font scales x and y
                                                       0, 0, // subpixel shift x and y
                                                       supersample, supersample, // oversample x and y
                                                       &sub_x, &sub_y,
                                                       cp);

        }

        s32 y_start = ascend+y_offset;
        s32 y_end   = ascend+y_offset+bmp_height_in_px;
        s32 x_start = x_offset;
        s32 x_end   = x_offset+bmp_width_in_px;

        s32 bytes_per_line = (out_font->char_px_width / 8 + (out_font->char_px_width % 8 ? 1 : 0));

        u8 *bit_ptr = &font_data[(cp - unicode_cp_start) * out_font->char_px_height * bytes_per_line];
        bit_ptr += y_start * bytes_per_line;
        bit_ptr += x_start / 8;

        s32 shift = 7-(x_start % 8);

        s32 y;
        for (y = max(0, y_start); y < min(y_end, char_height_in_px/supersample); ++y) {
            s32 x;
            for (x = max(0, x_start); x < min(x_end, char_width_in_px/supersample); ++x) {
                // uint8_t pixel = bitmap[bmp_width_in_px*(y-y_start)+(x-x_start)];

#define ACCESS(_x, _y, _sx, _sy) bitmap_memory[(bmp_width_in_px*(supersample*(_y-y_start)+_sy)+supersample*(_x-x_start))+_sx];

                u32 avg = 0;
                // u32 hack_supersample = supersample;
                u32 hack_supersample = 1;
                for (u32 sub_y = 0; sub_y < hack_supersample; ++sub_y) {
                    for (u32 sub_x = 0; sub_x < hack_supersample; ++sub_x) {
                        avg += ACCESS(x, y, sub_x, sub_y);
                    }
                }

                u8 pixel = avg / (hack_supersample*hack_supersample);
                u8 bit = pixel >= gray_threashold;

                *bit_ptr |= (bit << shift);

                --shift;
                if (shift == -1) {
                    shift = 7;
                    ++bit_ptr;
                }
            }

            bit_ptr += (s32)((bytes_per_line*8-x-1) / 8) + 1;
            bit_ptr += (s32)(x_start/8.0);

            shift = 7-(x_start % 8);
        }
    }


    return Pixel_Font_Baker_Error::SUCCESS;
}

void destroy_pixel_font(Pixel_Font* font) {
    free(font->table);
}

#undef min
#undef max
#endif
