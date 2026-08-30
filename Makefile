PKGS := hyprland pixman-1 libdrm pangocairo cairo wayland-server xkbcommon
CXXFLAGS := -std=c++23 -O2 -g -fPIC -Wall -Wextra -Wno-unused-parameter \
            $(shell pkg-config --cflags $(PKGS))
LDFLAGS := $(shell pkg-config --libs $(PKGS))

hyprtabbed.so: src/main.cpp
	$(CXX) -shared --no-gnu-unique $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f hyprtabbed.so

.PHONY: clean
