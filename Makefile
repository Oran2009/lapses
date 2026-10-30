# The instrument, from one set of sources.
#
#   make        the PC build, with a window:  host/build/lapses.exe
#   make run    build it and start it
#   make test   build it and run its self test
#   make daisy  the Touch 2 build:            firmware/build/lapses.bin
#
# Both shells link the same engine. Only the shell differs: host/main_win.cpp
# puts a window and waveOut around it, firmware/src/main.cpp puts the Touch 2
# panel and the audio callback around it. There is no second implementation of
# anything, so there is nothing to keep in step.

ENGINE = firmware/src/wfc.cpp firmware/src/voices.cpp firmware/src/synth.cpp

VCVARS = C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat

# cl.exe needs the vcvars environment, and only cmd can source a .bat. Going
# through powershell is what survives git-bash's path mangling.
CL = cl /nologo /O2 /EHsc /W3 /Ifirmware\src /Fe:host\build\lapses.exe \
     /Fo:host\build\ host\main_win.cpp host\selftest.cpp \
     firmware\src\wfc.cpp firmware\src\voices.cpp firmware\src\synth.cpp

.DEFAULT_GOAL := host

.PHONY: host
host:
	@mkdir -p host/build
	@powershell -NoProfile -Command "cmd /c '\"$(VCVARS)\" >nul 2>&1 && $(CL)'"

.PHONY: run
run: host
	@./host/build/lapses.exe

.PHONY: test
test: host
	@./host/build/lapses.exe --selftest

.PHONY: daisy
daisy:
	$(MAKE) -C firmware

.PHONY: clean
clean:
	rm -rf host/build firmware/build
