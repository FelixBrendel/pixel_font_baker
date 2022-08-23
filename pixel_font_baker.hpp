#include <ftb/print.hpp>
#include <ftb/types.hpp>
#include <ftb/macros.hpp>

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
    STB_TRUETYPE_FAILED,
};

Pixel_Font_Baker_Error create_pixel_font(const char* font_path, u16 char_height_in_px,
                                         u32 unicode_cp_start, u32 unicode_cp_end,
                                         u8 gray_threashold, u8 prefilter, Pixel_Font* out_font);

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

Pixel_Font_Baker_Error create_pixel_font(const char* font_path, u16 char_height_in_px,
                                         u32 unicode_cp_start, u32 unicode_cp_end,
                                         u8 gray_threashold, u8 prefilter, Pixel_Font* out_font)
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


    //
    //  Preparing one-char-bitmap
    //
    u8* bitmap;
    s32 unicode_cp_size  = unicode_cp_end - unicode_cp_start + 1;

    s32 char_width_in_px;
    f32 font_scale  = stbtt_ScaleForPixelHeight(&font, char_height_in_px);
    stbtt_GetCodepointHMetrics(&font, 'W', &char_width_in_px, nullptr);
    char_width_in_px = (s32)ceil(char_width_in_px*font_scale);

    uint8_t* bitmap_memory = (uint8_t*)malloc(char_height_in_px*char_width_in_px);
    if (!bitmap_memory)
        return Pixel_Font_Baker_Error::MALLOC_FAILED;

    //
    //  Preparing pixel-font-array
    //

    // get number of bytes per pixel line per char
    s32 bytes_per_line =
        (char_width_in_px % 8 == 0) ?
        (char_width_in_px / 8)      :
        ((char_width_in_px / 8) + 1);

    s32 ascend;
    s32 descend;
    s32 line_gap;
    stbtt_GetFontVMetrics(&font, &ascend, &descend, &line_gap);
    ascend = (int)(ascend*font_scale +.5f);
    descend = char_height_in_px - ascend;

    s32 total_byte_size = unicode_cp_size * char_height_in_px * bytes_per_line;
    u8* font_data = (u8*)malloc(total_byte_size);
    if (!font_data)
        return Pixel_Font_Baker_Error::MALLOC_FAILED;

    memset((void*)font_data, 0, total_byte_size);

    //
    //  Fill the pixel-font
    //

    out_font->char_px_width   = char_width_in_px;
    out_font->char_px_height  = char_height_in_px;
    out_font->table           = font_data;
    out_font->bytes_per_line  = bytes_per_line;
    out_font->bytes_per_glyph = bytes_per_line * char_height_in_px;

    log_debug("width:           %i", out_font->char_px_width);
    log_debug("height:          %i", out_font->char_px_height);
    log_debug("bytes per line:  %i", out_font->bytes_per_line);
    log_debug("bytes per glyph: %i", out_font->bytes_per_glyph);

    for (u32 cp = unicode_cp_start; cp <= unicode_cp_end; ++cp) {
        // printf("Generating letter for cp: %i\n", cp);

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
            // stbtt_MakeCodepointBitmap(&font, bitmap_memory,
                                      // char_width_in_px, char_height_in_px,
                                      // char_width_in_px, // TODO(Felix): stride
                                      // font_scale, font_scale, // font scales x and y
                                      // cp);

            f32 sub_x, sub_y;
            stbtt_MakeCodepointBitmapSubpixelPrefilter(&font, bitmap_memory,
                                                       char_width_in_px, char_height_in_px,
                                                       char_width_in_px, // TODO(Felix): stride
                                                       font_scale, font_scale, // font scales x and y
                                                       0, 0, // subpixel shift x and y
                                                       prefilter, prefilter, // oversample x and y
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
        for (y = max(0, y_start); y < min(y_end, char_height_in_px); ++y) {
            s32 x;
            for (x = max(0, x_start); x < min(x_end, char_width_in_px); ++x) {
                // uint8_t pixel = bitmap[bmp_width_in_px*(y-y_start)+(x-x_start)];
                u8 pixel = bitmap_memory[bmp_width_in_px*(y-y_start)+(x-x_start)];
                u8 bit   = pixel >= gray_threashold;

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
