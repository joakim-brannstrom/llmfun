.PHONY: all build/tui clean

all: build/tui

build/tui.stamp:
	mkdir -p build/tui
	cd build/tui && cmake ../../cpp_tui
	touch build/tui.stamp

build/tui: build/tui.stamp
	cd build/tui && make -j $(nproc)

clean:
	rm -rf build/tui*

