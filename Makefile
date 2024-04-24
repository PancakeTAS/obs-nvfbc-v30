TARGET = obs-nvfbc

CC = gcc
CFLAGS = -Wno-unused-parameter -Wall -Wextra -Werror -std=gnu17 -pedantic -Iinclude
LDFLAGS = -nostartfiles -shared -fPIC -Wl,--entry=lib_main
LIBS = -lnvidia-fbc -ldl -lobs

ifndef PROD
CFLAGS += -g
else
CFLAGS += -O3 -march=native -mtune=native
LDLAGS += -flto=auto
endif

$(TARGET).so: $(TARGET).c
	$(CC) $(CFLAGS) $(LIBS) $(LDFLAGS) $^ -o $@

link: $(TARGET).so
	mkdir -p "$(HOME)/.config/obs-studio/plugins/$(TARGET)/bin/64bit"
	ln -s "$(PWD)/$(TARGET).so" "$(HOME)/.config/obs-studio/plugins/$(TARGET)/bin/64bit/$(TARGET).so"

run: $(TARGET).so
	obs

debug: $(TARGET).so
	gdb obs

clean:
	rm -f $(TARGET).so

.PHONY: link run debug clean