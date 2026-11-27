#ifndef LUCYK_IO_FBCON_H
#define LUCYK_IO_FBCON_H

#include <types.h>

/* Linear-framebuffer 8x16 text console. Renders into the Limine framebuffer
 * alongside COM1 serial; disabled when no framebuffer is available. */

/* Initializes from a Limine framebuffer (already addressable). width/height/
 * pitch in pixels/bytes, bpp bits per pixel, channel masks+shifts for RGB.
 * No-op if addr is NULL or dimensions are unusable. */
void fbcon_init(void *addr, uint32_t width, uint32_t height, uint32_t pitch,
                uint32_t bpp, uint8_t red_mask_size, uint8_t red_shift,
                uint8_t green_mask_size, uint8_t green_shift,
                uint8_t blue_mask_size, uint8_t blue_shift);

void fbcon_putc(char c);
void fbcon_write(const char *s, size_t len);

/* Atomic handover: kernel text drawing stops cleanly without tearing once a
 * Ring 3 window server / GPU driver takes ownership of the display. */
void fbcon_release(void);
bool fbcon_active(void);

#endif /* LUCYK_IO_FBCON_H */
