#include <SDL.h>
#include <getopt.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "semu.h"
#include "framebuffer.h"
#include "screen.h"

extern struct cpu *cpu;

void draw_framebuffer(struct Screen *screen) {
    SDL_Texture *texture = ScreenGetTexture(screen);
    SDL_UpdateTexture(texture, NULL, &cpu->bus->ram->data[FRAMEBUFFER_BASE - RAM_BASE], FRAMEBUFFER_WIDTH * 4);
}
