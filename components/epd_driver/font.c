#include "epd_driver.h"
#include "esp_assert.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lib/zlib/zlib.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint8_t mask;    /* char data will be bitwise AND with this */
  uint8_t lead;    /* start bytes of current char in utf-8 encoded character */
  uint32_t beg;    /* beginning of codepoint range */
  uint32_t end;    /* end of codepoint range */
  int bits_stored; /* the number of bits from the codepoint that fits in char */
} utf_t;

/*
 * UTF-8 decode inspired from rosetta code
 * https://rosettacode.org/wiki/UTF-8_encode_and_decode#C
 */
utf_t *utf[] = {
    /*             mask        lead        beg      end       bits */
    [0] = &(utf_t){0b00111111, 0b10000000, 0, 0, 6},
    [1] = &(utf_t){0b01111111, 0b00000000, 0000, 0177, 7},
    [2] = &(utf_t){0b00011111, 0b11000000, 0200, 03777, 5},
    [3] = &(utf_t){0b00001111, 0b11100000, 04000, 0177777, 4},
    [4] = &(utf_t){0b00000111, 0b11110000, 0200000, 04177777, 3},
    &(utf_t){0},
};

int utf8_len(const uint8_t ch) {
  int len = 0;
  for (utf_t **u = utf; *u; ++u) {
    if ((ch & ~(*u)->mask) == (*u)->lead) {
      break;
    }
    ++len;
  }
  if (len > 4) { /* Malformed leading byte */
    assert("invalid unicode.");
  }
  return len;
}

uint32_t next_cp(uint8_t **string) {
  if (**string == 0) {
    return 0;
  }
  int bytes = utf8_len(**string);
  uint8_t *chr = *string;
  *string += bytes;
  int shift = utf[0]->bits_stored * (bytes - 1);
  uint32_t codep = (*chr++ & utf[bytes]->mask) << shift;

  for (int i = 1; i < bytes; ++i, ++chr) {
    shift -= utf[0]->bits_stored;
    codep |= ((uint8_t)*chr & utf[0]->mask) << shift;
  }

  return codep;
}

void get_glyph(GFXfont *font, uint32_t code_point, GFXglyph **glyph) {
  UnicodeInterval *intervals = font->intervals;
  *glyph = NULL;
  for (int i = 0; i < font->interval_count; i++) {
    UnicodeInterval *interval = &intervals[i];
    if (code_point >= interval->first && code_point <= interval->last) {
      *glyph = &font->glyph[interval->offset + (code_point - interval->first)];
      return;
    }
    if (code_point < interval->first) {
      return;
    }
  }
  return;
}

/*!
   @brief   Draw a single character to a pre-allocated buffer.
*/
void draw_char(GFXfont *font, uint8_t *buffer, int *cursor_x, int cursor_y, uint16_t buf_width,
              uint16_t buf_height, uint32_t cp) {

  GFXglyph *glyph;
  get_glyph(font, cp, &glyph);

  // TODO: Draw Tofu character
  if (!glyph) {
    return;
  }

  uint32_t offset = glyph->data_offset;
  uint8_t width = glyph->width, height = glyph->height;
  int left = glyph->left;

  unsigned long bitmap_size = width * height;
  uint8_t *bitmap = (uint8_t *)malloc(bitmap_size);
  uncompress(bitmap, &bitmap_size, &font->bitmap[offset],
             glyph->compressed_size);

  for (uint32_t i = 0; i < bitmap_size; i++) {
    int xx = *cursor_x + left + i % width;
    int yy = cursor_y - glyph->top + i / width;
    if (xx < 0 || xx / 2 + (xx % 2) >= buf_width) {
        continue;
    }
    if (yy < 0 || yy >= buf_height) {
        continue;
    }
    uint32_t buf_pos = yy * buf_width + xx / 2 + (xx % 2);
    if (xx % 2) {
      buffer[buf_pos] = (buffer[buf_pos] & 0xF0) | (bitmap[i] >> 4);
    } else {
      buffer[buf_pos] = (buffer[buf_pos] & 0x0F) | (bitmap[i] & 0xF0);
    }
  }
  free(bitmap);
  *cursor_x += glyph->advance_x;
}

/*!
 * @brief Calculate the bounds of a character when drawn at (x, y), move the
 * cursor (*x) forward, adjust the given bounds.
 */
void get_char_bounds(GFXfont *font, uint32_t cp, int *x, int *y, int *minx,
                   int *miny, int *maxx, int *maxy) {
  GFXglyph *glyph;
  get_glyph(font, cp, &glyph);

  // TODO: Draw Tofu character
  if (!glyph) {
    return;
  }

  int x1 = *x + glyph->left, y1 = *y + (glyph->top - glyph->height),
      x2 = x1 + glyph->width, y2 = y1 + glyph->height;
  if (x1 < *minx)
    *minx = x1;
  if (y1 < *miny)
    *miny = y1;
  if (x2 > *maxx)
    *maxx = x2;
  if (y2 > *maxy)
    *maxy = y2;
  *x += glyph->advance_x;
}

int min(int x, int y) { return x < y ? x : y; }

void get_text_bounds(GFXfont *font, char *string, int x, int y, int *x1,
                     int *y1, int *w, int *h) {
  int xx = x, yy = y, minx = 100000, miny = 100000, maxx = -1, maxy = -1;
  uint32_t c;
  while ((c = next_cp((uint8_t**)&string))) {
    get_char_bounds(font, c, &xx, &yy, &minx, &miny, &maxx, &maxy);
  }
  *x1 = min(x, minx);
  *w = maxx - *x1;
  *y1 = miny;
  *h = maxy - miny;
}

void writeln(GFXfont *font, char *string, int *cursor_x, int *cursor_y,
             uint8_t *framebuffer) {

  int x1 = 0, y1 = 0, w = 0, h = 0;
  get_text_bounds(font, string, *cursor_x, *cursor_y, &x1, &y1, &w, &h);

  uint8_t *buffer;
  int buf_width;
  int buf_height;
  int baseline_height = *cursor_y - y1;

  // The local cursor position:
  // 0, if drawing to a local temporary buffer
  // the given cursor position, if drawing to a full frame buffer
  int local_cursor_x = 0;
  int local_cursor_y = 0;

  if (framebuffer == NULL) {
    buf_width = (w / 2 + w % 2);
    buf_height = h;
    buffer = (uint8_t *)malloc(buf_width * buf_height);
    memset(buffer, 255, buf_width *buf_height);
    local_cursor_y = buf_height - baseline_height;
  } else {
    buf_width = EPD_WIDTH / 2;
    buf_height = EPD_HEIGHT;
    buffer = framebuffer;
    local_cursor_x = *cursor_x;
    local_cursor_y = *cursor_y;
  }

  uint32_t c;

  while ((c = next_cp((uint8_t**)&string))) {
    draw_char(font, buffer, &local_cursor_x, local_cursor_y, buf_width, buf_height, c);
  }

  if (framebuffer == NULL) {
    Rect_t area = {
      .x = x1, .y = *cursor_y - h + baseline_height, .width = w, .height = h};

    epd_draw_grayscale_image(area, buffer);

    free(buffer);
  }
}

void write_string(GFXfont *font, char *string, int *cursor_x, int *cursor_y,
             	  uint8_t *framebuffer) {
   // taken from the strsep manpage
   char *token, *newstring, *tofree;

   tofree = newstring = strdup(string);
   if (string == NULL) {
	   ESP_LOGE("font.c", "cannot allocate string copy!");
	   return;
   }

   int line_start = *cursor_x;
   while ((token = strsep(&newstring, "\n")) != NULL) {
	   *cursor_x = line_start;
	   writeln(font, token, cursor_x, cursor_y, framebuffer);
	   *cursor_y += font->advance_y;
   }

   free(tofree);
}
