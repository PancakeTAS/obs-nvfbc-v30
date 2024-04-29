SOURCES = $(wildcard src/*.c)
OBJECTS = $(SOURCES:.c=.o)

TARGET = obs-nvfbc.so

CC = gcc
CFLAGS = -Wno-unused-parameter -Wall -Wextra -std=gnu17 -Iinclude -fPIC
LDFLAGS = -shared
LIBS = -lnvidia-fbc -ldl -lobs -lEGL

ifndef PROD
CFLAGS += -g
else
CFLAGS += -O3 -march=native -mtune=native
LDLAGS += -flto=auto
endif

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LIBS) $(LDFLAGS) $^ -o $@

link: $(TARGET)
	mkdir -p "$(HOME)/.config/obs-studio/plugins/$(TARGET)/bin/64bit"
	ln -s "$(PWD)/$(TARGET)" "$(HOME)/.config/obs-studio/plugins/$(TARGET)/bin/64bit/$(TARGET)"

run: $(TARGET)
	obs

debug: $(TARGET)
	gdb obs

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: link run debug clean
