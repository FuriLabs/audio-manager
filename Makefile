CC = gcc

CFLAGS = -fPIC -Iinclude -I. `pkg-config --cflags glib-2.0 alsa libxml-2.0`
LDFLAGS += `pkg-config --libs glib-2.0 alsa libxml-2.0`

PREFIX ?= /usr
INCLUDEDIR ?= $(PREFIX)/include
DATADIR ?= $(PREFIX)/share
CONFIGDIR ?= $(DATADIR)/audio-manager

TRIPLET ?= $(shell $(CC) -dumpmachine)
LIBDIR ?= $(PREFIX)/lib/$(TRIPLET)

BACKEND_MTK := \
	backends/mtk/mtk-audio-manager.c \
	backends/mtk/mtk-config.c \
	backends/mtk/params/mtk-audio-param.c \
	backends/mtk/params/mtk-speech-param.c \
	backends/mtk/params/mtk-speech-volume.c \
	backends/mtk/route/mtk-device-parser.c \
	backends/mtk/speech/mtk-ccci.c \
	backends/mtk/speech/mtk-ccci-shm.c \
	backends/mtk/speech/mtk-speech.c \
	backends/mtk/speech/mtk-speech-gen97.c \
	backends/mtk/speech/mtk-speech-generation.c \
	backends/mtk/speech/mtk-speech-protocol.c \
	backends/mtk/speech/mtk-usip.c
BACKEND := $(BACKEND_MTK)

C_SRCS := \
	src/audio-manager.c \
	src/audio-manager-error.c \
	common/audio-format.c \
	common/audio-device.c \
	common/audio-state.c \
	common/math-utils.c \
	common/xml-utils.c \
	common/route.c \
	common/config/config.c \
	common/mainloop/fd-source.c \
	common/alsa/alsa-card.c \
	common/alsa/alsa-control.c \
	common/alsa/alsa-pcm.c \
	backends/backend.c \
	$(BACKEND)

OBJS := $(C_SRCS:.c=.o)

TARGET := libaudio-manager.so
TARGET_CTL := audio-manager-ctl

.PHONY: all clean install

all: $(TARGET) $(TARGET_CTL)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -shared -o $@ $^ $(LDFLAGS)

$(TARGET_CTL): tools/audio-manager-ctl.o $(TARGET)
	$(CC) $(LDFLAGS) -o $@ tools/audio-manager-ctl.o -L. -laudio-manager $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0644 $(TARGET) $(DESTDIR)$(LIBDIR)/$(TARGET)
	install -d $(DESTDIR)$(INCLUDEDIR)/audio-manager
	install -m 0644 include/audio-manager/*.h $(DESTDIR)$(INCLUDEDIR)/audio-manager/
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(TARGET_CTL) $(DESTDIR)$(PREFIX)/bin/$(TARGET_CTL)
	install -d $(DESTDIR)$(CONFIGDIR)/devices
	install -m 0644 configs/audio-manager.conf $(DESTDIR)$(CONFIGDIR)/audio-manager.conf
	install -m 0644 configs/devices/*.conf $(DESTDIR)$(CONFIGDIR)/devices/

clean:
	rm -f $(OBJS) tools/audio-manager-ctl.o $(TARGET) $(TARGET_CTL)
