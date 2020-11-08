alsaimpulse
===========

An ALSA plugin to apply arbitrary convolution filters to PCM
streams. Released under the GNU GPLv3+.

FFTW is used with the [overlap-add
method](https://en.wikipedia.org/wiki/Overlap%E2%80%93add_method) to
convolve the two signals in reasonable CPU time.

**CONSIDER THIS PLUGIN EXPERIMENTAL** until the maths have been
checked out by someone who knows what they are doing.

Use cases
=========

* DRC (digital room correction: room EQ, house curves, etc.)
* Other types of FIR filters

This plugin does not require the use of an ALSA loopback device. This
is great for many reasons:

* No unnecessary latency (of which ALSA programs cannot know about,
  this is problematic for precise syncing of audio). Of course,
  because of the way convolution filters work, some latency is
  inevitable.

* All sample rates are supported without the need for continous
  resampling (only the impulse response is resampled, once), which
  saves system resources during playback.

* No idle use of system resources (the device is normally closed when
  no application is playing audio).

Generating impulses
===================

For DRC, you can generate impulses using software like
[DRC-FIR](https://sourceforge.net/projects/drc-fir/) (libre) or
[REW](https://www.roomeqwizard.com/) (proprietary).

For more general-purpose filters, you can generate impulses with
ffmpeg's [sinc source](https://ffmpeg.org/ffmpeg-filters.html#sinc)
(among others):

~~~
# generate a 100 Hz highpass filter impulse at 44100 Hz sample rate
ffmpeg -f lavfi -i sinc=r=44100:hp=100 -f f32le highpass_impulse.float32.pcm
~~~

Finally, you can also reuse impulses available online, some of which
can be found on [zconvolver's
homepage](https://x42-plugins.com/x42/x42-zconvolver).

Limitations
===========

* No control mixer for toggling the filter on/off for now, patches
  welcome

* 16 channels maximum (can easily be increased in the code)

* Only works with `FLOAT32` samples of native endianness, you will
  most likely need to use `plug` slaves to feed data in and out

* Because `dmix` requires a `hw` slave, one instance of the plugin
  will run for each process playing audio. This is inefficient if you
  have two (or more) processes that play audio concurrently. This only
  matters if you use `dmix`.

* CPU usage will be significant if you use large impulses or short
  period sizes.

Compile and install
===================

Dependencies: FFTW and libsamplerate (both lib and lib32 versions).

~~~
make
sudo make install
~~~

Example configuration (`asoundrc`)
==================================

The plugin requires (and assumes) impulse files are raw PCM data
stored as 32-bit floats of native endianness. If your impulse files
are in a different format, you can convert them with tools like
`ffmpeg(1)`: `ffmpeg -i impulse.wav -f f32le impulse.float32.pcm`

~~~
pcm.impulse {
	type impulse
	slave { pcm "plughw:0" }

	# This is optional but increases CPU efficiency (except the very first time the plugin is loaded)
	# Specified path must be writeable
	# Recommended to delete the file after hardware changes / software upgrades
	wisdom_path "/home/foo/.cache/alsa/impulse_wisdom"

	impulse.0 {
		path "/path/to/left_channel.float32.pcm"
		rate 44100
		gain -20.0 # preamp gain in dB, optional (default 0.0 dB)
	}

	impulse.1 {
		path "/path/to/right_channel.float32.pcm"
		rate 44100
		gain -20.0
	}

	# add more channels as needed...
	#impulse.2 {
	#	path "/path/to/center_channel.float32.pcm"
	#	rate 44100
	#	gain -20.0
	#	fft_size 16384 # for experts only, omit for optimal results
	#}
}
pcm.!default {
	type plug
	slave { pcm impulse }
}
~~~

See also
========

* [BruteFIR](https://torger.se/anders/brutefir.html) and
  [CamillaDSP](https://github.com/HEnquist/camilladsp), both of which
  can do convolution filters in real-ish time using a loopback device

* [alsaloudness](https://github.com/dpapavas/alsaloudness), a
  loudness-compensated volume control for ALSA, must-have if you are
  listening at low SPLs

* [ir.lv2](https://tomszilagyi.github.io/plugins/ir.lv2/) and
  [zeroconvo.lv2](https://github.com/x42/zconvo.lv2), which might be
  more adequate for hard real-time processing with JACK

Licence
=======

`alsaimpulse` is released under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3
of the License, or (at your option) any later version. See the file
`COPYING` for more details.
