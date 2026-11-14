# Browser (WebAssembly) build of st3.  Needs Emscripten's emcc on PATH (source emsdk_env.sh first).
# Output: build/ = index.html, st3.js, st3-worker.js, audio-worklet.js, st3core.js/.wasm/.data — serve it with any static web server.
CORE = $(addprefix ../st3emu/src/,cpu.c machine.c io.c vga.c bios.c dos.c keys.c opl.c mixer.c vgafb.c ems.c st3exe_data.c)
EXPORTS = _st3_init,_st3_frame,_st3_render,_st3_pixels,_st3_width,_st3_height,_st3_key,_st3_shift,_st3_audio_take,_st3_audio_buf,_st3_exited
DIST = ../dist
EMFLAGS = -O3 -sENVIRONMENT=worker -sINITIAL_MEMORY=64MB -sALLOW_MEMORY_GROWTH=1 -sFORCE_FILESYSTEM=1 -lidbfs.js \
          -sEXPORTED_FUNCTIONS=$(EXPORTS) -sEXPORTED_RUNTIME_METHODS=FS,IDBFS,HEAPU8,HEAPF32 \
          --preload-file $(DIST)/s@/dist/s --preload-file $(DIST)/i@/dist/i --preload-file $(DIST)/ST3.SET@/dist/ST3.SET \
          --preload-file '$(DIST)/\#\#\#\#\#\#\#\#.ST3@/dist/\#\#\#\#\#\#\#\#.ST3'

all: build/st3core.js build/index.html build/st3.js build/st3-worker.js build/audio-worklet.js

../st3emu/src/st3exe_data.c:
	$(MAKE) -C ../st3emu src/st3exe_data.c

build/st3core.js: st3web.c $(CORE) ../st3emu/src/*.h $(wildcard $(DIST)/s/*) $(wildcard $(DIST)/i/*)
	@mkdir -p build
	emcc $(EMFLAGS) -o $@ st3web.c $(CORE)

build/%: %
	@mkdir -p build
	cp $< $@

serve: all
	cd build && python3 -m http.server 8321

clean:
	rm -rf build
.PHONY: all serve clean
