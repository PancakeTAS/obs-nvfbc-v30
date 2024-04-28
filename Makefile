TARGET = obs-nvfbc

CC = gcc
CFLAGS = -Wno-unused-parameter -Wall -Wextra -std=gnu17 -Iinclude
LDFLAGS = -shared -fPIC
LIBS = -lnvidia-fbc -ldl -lobs -lEGL

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
