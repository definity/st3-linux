CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation
libst3cpu.so: src/cpu.c src/cpu.h tests/cpu_api.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ src/cpu.c tests/cpu_api.c
clean:
	rm -f libst3cpu.so st3 st3head opl_test

CORE = src/cpu.c src/machine.c src/io.c src/vga.c src/bios.c src/dos.c src/keys.c src/opl.c src/mixer.c
st3head: $(CORE) src/*.h tests/headless.c
	$(CC) $(CFLAGS) -o $@ $(CORE) tests/headless.c -lm

opl_test: src/opl.c src/opl.h tests/opl_test.c
	$(CC) $(CFLAGS) -o $@ tests/opl_test.c src/opl.c -lm

src/st3exe_data.c: ../ST3U.EXE tools_embed.py
	python3 -I tools_embed.py ../ST3U.EXE $@

APP = $(CORE) src/render.c src/audio.c src/main.c src/st3exe_data.c
st3: $(APP) src/*.h
	$(CC) $(CFLAGS) -static -o $@ $(APP) -lm
