TARGET = obs-nvfbc

CC = gcc
CFLAGS = -Wno-unused-parameter -Wall -Wextra -Werror -std=gnu17 -pedantic -Iinclude
LDFLAGS = -lnvidia-fbc -ldl -lobs

ifndef PROD
CFLAGS += -g
else
CFLAGS += -O3 -march=native -mtune=native
LDLAGS += -flto=auto
endif

all: $(TARGET).so $(TARGET).o

$(TARGET).so: $(TARGET).c
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -fPIC $^ -o $@

$(TARGET).o: $(TARGET).c
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@


link: $(TARGET).so $(TARGET).o
	ln -s "$(PWD)/$(TARGET).so" "$(HOME)/.config/obs-studio/plugins/$(TARGET)/bin/64bit/$(TARGET).so"
	ln -s "$(PWD)/$(TARGET).o" "$(HOME)/.config/obs-studio/plugins/$(TARGET)/bin/64bit/$(TARGET).o"

run: $(TARGET).so $(TARGET).o
	obs

debug: $(TARGET).so $(TARGET).o
	gdb obs

clean:
	rm -f $(TARGET).so $(TARGET).o

.PHONY: link run debug clean
