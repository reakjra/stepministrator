CC       = x86_64-w64-mingw32-gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -municode
LDFLAGS ?= -mwindows -s
LDLIBS  ?= -lwinhttp -lntdll -luser32

TARGET  := stepministrator.exe
SOURCES := stepministrator.c vendor/puff.c

$(TARGET): $(SOURCES) vendor/puff.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: clean
