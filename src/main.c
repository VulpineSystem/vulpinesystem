#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "framebuffer.h"
#include "screen.h"
#include "semu.h"

#define FPS 60
#define TPF 1
#define TPS (FPS * TPF)

struct cpu *cpu;

uint32_t tick_start;
uint32_t tick_end;
int ticks = 0;
bool done = false;

void main_loop(void);
void execute_instruction(void);

int main(int argc, char *argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fatal("initialize SDL");
        return 1;
    }

    SDL_ShowCursor(SDL_DISABLE);

    if (argc < 2) {
        printf("Usage: %s <raw kernel image> [<disk image>]\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f)
        fatal("open raw kernel image");

    uint8_t *binary = NULL, *disk = NULL;
    size_t fsize = read_file(f, &binary);
    fclose(f);

    if (argc == 3) {
        f = fopen(argv[2], "rb");
        if (!f)
            fatal("open disk image");
        read_file(f, &disk);
        fclose(f);
    }

    cpu = cpu_new(binary, fsize, disk);
    free(binary);

    ScreenCreate(
        FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT,
        draw_framebuffer,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL
    );

    ScreenInit();
    ScreenDraw();

    tick_start = SDL_GetTicks();
    tick_end = SDL_GetTicks();

    while (!done) {
        main_loop();

        tick_end = SDL_GetTicks();
        int delay = 1000/TPS - (tick_end - tick_start);
        if (delay > 0) {
            SDL_Delay(delay);
        } else {
            //printf("time overrun %d\n", delay);
        }
    }

    return 0;
}

void main_loop(void) {
    int dt = SDL_GetTicks() - tick_start;
    tick_start = SDL_GetTicks();
    if (!dt)
        dt = 1;

    int cycles_per_tick = CPU_HZ / TPS / dt;
    int extra_cycles = CPU_HZ / TPS - (cycles_per_tick * dt);

    for (int i = 0; i < dt; i++) {

        int cycles_left = cycles_per_tick;

        if (i == dt - 1)
            cycles_left += extra_cycles;

        while (cycles_left > 0) {
            execute_instruction();
            cycles_left--;
        }
    }

    if ((ticks % TPF) == 0) {
        ScreenDraw();
    }

    done = ScreenProcessEvents();

    ticks++;
}

void execute_instruction(void) {
    // fetch instruction
    uint64_t insn;
    exception_t e;
    if ((e = cpu_fetch(cpu, &insn)) != OK) {
        cpu_take_trap(cpu, e, NONE);
        if (exception_is_fatal(e)) {
            printf("fatal exception while fetching instruction!");
            exit(0);
        }
        insn = 0;
    }

    //printf("%" PRIX64 ": %" PRIX64 "\n", cpu->pc, insn);

    // advance pc
    cpu->pc += 4;

    // decode and execute
    if ((e = cpu_execute(cpu, insn)) != OK) {
        cpu_take_trap(cpu, e, NONE);
        if (exception_is_fatal(e)) {
            printf("fatal exception while executing instruction!");
            exit(0);
        }
    }

    interrupt_t intr;
    if ((intr = cpu_check_pending_interrupt(cpu)) != NONE)
        cpu_take_trap(cpu, OK, intr);
}
