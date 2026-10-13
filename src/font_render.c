#include "font_render.h"
#include "font5x7.h"

/* Setzt ein einzelnes Pixel im 1bpp-Framebuffer.
 * Layout: Zeile fuer Zeile, je Zeile (fb_width+7)/8 Byte, MSB = linkestes Pixel. */
static void fb_set_pixel(uint8_t *fb, uint16_t fb_width, uint16_t fb_height,
			 uint16_t x, uint16_t y, uint8_t color)
{
	if (x >= fb_width || y >= fb_height) {
		return;
	}
	size_t bytes_per_row = (fb_width + 7) / 8;
	size_t byte_idx = y * bytes_per_row + (x / 8);
	uint8_t bit_mask = 0x80 >> (x % 8);

	/* Laut Microtech-Datenblatt (Beispiel im "LUT from OTP Operation Flow"):
	 * ein Framebuffer aus lauter 0x00-Bytes zeigt "all black" - Bit=0 ist
	 * also schwarz, Bit=1 weiss. Umgekehrt zur vorherigen (falschen) Annahme. */
	if (color) {
		fb[byte_idx] &= ~bit_mask; /* schwarz */
	} else {
		fb[byte_idx] |= bit_mask;  /* weiss */
	}
}

void font_draw_char(uint8_t *fb, uint16_t fb_width, uint16_t fb_height,
		    uint16_t x, uint16_t y, char c, uint8_t scale, uint8_t color)
{
	if (c < FONT5X7_FIRST_CHAR || c > FONT5X7_LAST_CHAR) {
		c = ' '; /* unbekannte Zeichen als Leerzeichen */
	}
	const uint8_t *glyph = font5x7[c - FONT5X7_FIRST_CHAR];

	for (uint8_t col = 0; col < FONT5X7_WIDTH; col++) {
		uint8_t column_bits = glyph[col];
		for (uint8_t row = 0; row < FONT5X7_HEIGHT; row++) {
			if (column_bits & (1 << row)) {
				/* ein Font-Pixel auf scale x scale Ausgabepixel aufblasen */
				for (uint8_t sx = 0; sx < scale; sx++) {
					for (uint8_t sy = 0; sy < scale; sy++) {
						fb_set_pixel(fb, fb_width, fb_height,
							    x + col * scale + sx,
							    y + row * scale + sy,
							    color);
					}
				}
			}
		}
	}
}

void font_draw_string(uint8_t *fb, uint16_t fb_width, uint16_t fb_height,
		      uint16_t x, uint16_t y, const char *str, uint8_t scale, uint8_t color)
{
	uint16_t cursor_x = x;
	const uint16_t char_advance = (FONT5X7_WIDTH + 1) * scale; /* +1 Pixel Abstand */

	while (*str != '\0') {
		font_draw_char(fb, fb_width, fb_height, cursor_x, y, *str, scale, color);
		cursor_x += char_advance;
		str++;
	}
}

void font_draw_bitmap(uint8_t *fb, uint16_t fb_width, uint16_t fb_height,
		      uint16_t x, uint16_t y, const uint8_t *bitmap,
		      uint16_t bitmap_width, uint16_t bitmap_height)
{
	size_t bitmap_bytes_per_row = (bitmap_width + 7) / 8;

	for (uint16_t by = 0; by < bitmap_height; by++) {
		for (uint16_t bx = 0; bx < bitmap_width; bx++) {
			size_t src_byte_idx = by * bitmap_bytes_per_row + (bx / 8);
			uint8_t src_bit_mask = 0x80 >> (bx % 8);
			uint8_t pixel_is_white = (bitmap[src_byte_idx] & src_bit_mask) != 0;

			/* color-Parameter von fb_set_pixel: 1=schwarz setzen. Die
			 * Bitmap ist bereits in derselben bit=0-schwarz-Konvention
			 * gespeichert, deshalb hier direkt uebernehmen statt wie bei
			 * font_draw_char ueber ein gesetztes Font-Bit zu entscheiden. */
			fb_set_pixel(fb, fb_width, fb_height, x + bx, y + by, pixel_is_white ? 0 : 1);
		}
	}
}
