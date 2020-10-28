CC?=clang
CFLAGS=-std=c99 -shared -fPIC -DPIC -Wall -O3
LDFLAGS=-lasound -lfftw3f -lm -lsamplerate

all: libasound_module_pcm_impulse.so libasound_module_pcm_impulse32.so

install: all
	install -m755 libasound_module_pcm_impulse.so /usr/lib/alsa-lib/libasound_module_pcm_impulse.so
	install -m755 libasound_module_pcm_impulse32.so /usr/lib32/alsa-lib/libasound_module_pcm_impulse.so

uninstall:
	rm -f /usr/lib/alsa-lib/libasound_module_pcm_impulse.so /usr/lib32/alsa-lib/libasound_module_pcm_impulse.so

libasound_module_pcm_impulse.so: impulse_pcm.c
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $<

libasound_module_pcm_impulse32.so: impulse_pcm.c
	$(CC) -o $@ $(CFLAGS) -m32 $(LDFLAGS) $<

clean:
	rm -f libasound_module_pcm_impulse.so libasound_module_pcm_impulse32.so

.PHONY: all clean install uninstall
