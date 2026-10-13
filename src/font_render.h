#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zeichnet ein Zeichen in einen 1bpp-Framebuffer (MSB-first pro Byte,
 * Zeilen zu je width/8 Byte, Top-to-Bottom).
 * fb: Framebuffer, fb_width/fb_height: Gesamtaufloesung in Pixeln.
 * x,y: obere linke Ecke des Zeichens in Pixelkoordinaten.
 * scale: 1 = 5x7, 2 = 10x14 usw. (einfaches Pixel-Verdoppeln).
 * color: 1 = schwarz setzen, 0 = loeschen (weiss). */
void font_draw_char(uint8_t *fb, uint16_t fb_width, uint16_t fb_height,
		    uint16_t x, uint16_t y, char c, uint8_t scale, uint8_t color);

/* Zeichnet einen Null-terminierten String, zeichenweise mit font_draw_char,
 * fester Zeichenbreite (FONT5X7_WIDTH*scale + 1 Pixel Abstand). */
void font_draw_string(uint8_t *fb, uint16_t fb_width, uint16_t fb_height,
		      uint16_t x, uint16_t y, const char *str, uint8_t scale, uint8_t color);

/* Zeichnet eine 1bpp-Bitmap (gleiche Konvention wie fb: bit=0=schwarz,
 * bit=1=weiss, MSB-first pro Zeile) an Position x,y in den Framebuffer.
 * bitmap_width/bitmap_height: Groesse der Quellbitmap in Pixeln. */
void font_draw_bitmap(uint8_t *fb, uint16_t fb_width, uint16_t fb_height,
		      uint16_t x, uint16_t y, const uint8_t *bitmap,
		      uint16_t bitmap_width, uint16_t bitmap_height);

#ifdef __cplusplus
}
#endif
