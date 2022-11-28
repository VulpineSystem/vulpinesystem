SDL2_CONFIG = sdl2-config
CFLAGS = -g -Ofast -std=c99 -Wall -Wextra `$(SDL2_CONFIG) --cflags --libs`
TARGET=vulpinesystem

CFILES = src/main.c \
		src/framebuffer.c \
		src/keyboard.c \
		src/screen.c \
		src/semu.c

$(TARGET): $(CFILES)
	$(CC) -o $@ $(filter %.c, $^) $(CFLAGS)

clean:
	rm -rf vulpinesystem
