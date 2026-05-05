CC ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -Wall -O2

# gio-2.0 brings glib-2.0 + gobject-2.0 + GDBus in one go. gio-unix-2.0 is
# needed because Buildroot's pkg-config sometimes only lists unix helpers
# there even when we don't use them directly.
#
# Do NOT silence pkg-config's stderr — if it fails we want the build to
# explode with a clear message, not compile forward with empty -I.
GIO_CFLAGS := $(shell $(PKG_CONFIG) --cflags gio-2.0 gio-unix-2.0)
GIO_LIBS   := $(shell $(PKG_CONFIG) --libs   gio-2.0 gio-unix-2.0)

$(info [system_service] PKG_CONFIG=$(PKG_CONFIG))
$(info [system_service] GIO_CFLAGS=$(GIO_CFLAGS))
$(info [system_service] GIO_LIBS=$(GIO_LIBS))

ifeq ($(strip $(GIO_CFLAGS)),)
$(error pkg-config returned empty for gio-2.0. Is libglib2 built into staging? \
        Check: $(PKG_CONFIG) --list-all | grep -i glib)
endif

# Buildroot 调 make 时通过命令行传 CFLAGS=...,
# 命令行变量优先级最高, 普通 += 会被静默吞掉 -- 必须 override 才能追加.
# 否则 GIO_CFLAGS (glib/gio 的 -I 路径) 拼不进去, ble_gatt.c 找不到 glib.h.
override CFLAGS  += -pthread $(GIO_CFLAGS)
override LDFLAGS += -pthread

TARGET = system_service
OBJS = main.o log.o time_sync.o uart_log.o sys_stat.o \
       ble_gatt.o protocol.o wifi_switch.o \
       proto_dispatch.o unix_ipc.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIO_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

main.o:           main.c           log.h time_sync.h uart_log.h sys_stat.h ble_gatt.h wifi_switch.h unix_ipc.h
log.o:            log.c            log.h
time_sync.o:      time_sync.c      log.h time_sync.h
uart_log.o:       uart_log.c       log.h uart_log.h
sys_stat.o:       sys_stat.c       log.h sys_stat.h
ble_gatt.o:       ble_gatt.c       log.h ble_gatt.h protocol.h wifi_switch.h proto_sink.h proto_dispatch.h
protocol.o:       protocol.c       log.h ble_gatt.h protocol.h
wifi_switch.o:    wifi_switch.c    log.h wifi_switch.h
proto_dispatch.o: proto_dispatch.c log.h protocol.h proto_sink.h proto_dispatch.h wifi_switch.h
unix_ipc.o:       unix_ipc.c       log.h proto_sink.h proto_dispatch.h unix_ipc.h

clean:
	rm -f $(TARGET) *.o

install:
	install -D -m 0755 $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)
