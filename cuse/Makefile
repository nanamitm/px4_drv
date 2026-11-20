# SPDX-License-Identifier: GPL-2.0-only
# px4-dt295d: PIX-DT295 の CUSE デーモン。WinUSB 版と共有するコードは winusb/src/DriverHost_PX4 から参照する。
CC ?= cc
CXX ?= c++
PKG_CONFIG ?= pkg-config
BUILD ?= build
ROOT := ..
SHARED := $(ROOT)/winusb/src/DriverHost_PX4
DT295 := $(SHARED)/dt295
UPSTREAM := $(DT295)/upstream
FIRMWARE := $(ROOT)/winusb/pkg/DriverHost_PX4/firmware

PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
UNITDIR ?= /lib/systemd/system
UDEVDIR ?= /lib/udev/rules.d

DEPS_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0 fuse3)
DEPS_LIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0 fuse3)
CPPFLAGS += -I$(ROOT)/include -I$(SHARED) -I$(DT295) -I$(UPSTREAM)/include -I$(ROOT)/winusb/src/common -Isrc
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -pthread
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall

# TS の組み立て (上流の C 実装) と、WinUSB 版と共有する C++ のコード
UPSTREAM_OBJS := $(BUILD)/upstream/stream_input.o $(BUILD)/upstream/ts_framer.o $(BUILD)/upstream/ts_mux.o
# Windows 版の dt295/device.cpp と cuse/src/device.cpp の名前が重なるため、出力先を分ける。
SHARED_OBJS := $(BUILD)/shared/source_firmware.o $(BUILD)/shared/frontend_init.o $(BUILD)/shared/card_uart.o $(BUILD)/shared/control_channel.o
PLATFORM_OBJS := $(BUILD)/platform_linux.o $(BUILD)/firmware.o
DAEMON_OBJS := $(BUILD)/main.o $(BUILD)/device.o $(BUILD)/cuse_session.o $(BUILD)/usb_transport.o $(BUILD)/log.o

all: $(BUILD)/px4-dt295d

$(BUILD) $(BUILD)/shared $(BUILD)/upstream:
	mkdir -p $@

$(BUILD)/upstream/%.o: $(UPSTREAM)/core/%.c | $(BUILD)/upstream
	$(CC) $(CPPFLAGS) $(CFLAGS) -include $(DT295)/compat.h -c $< -o $@

$(BUILD)/shared/%.o: $(DT295)/%.cpp $(DT295)/*.hpp | $(BUILD)/shared
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.cpp src/*.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(DEPS_CFLAGS) $(CXXFLAGS) -c $< -o $@

# 埋め込むファームウェアが変わったら作り直す
$(BUILD)/firmware.o: src/firmware.S $(FIRMWARE)/dt295-direct-fr.bin $(FIRMWARE)/dt295-source-fr.bin | $(BUILD)
	$(CC) -c -Wa,-I$(FIRMWARE) $< -o $@

$(BUILD)/px4-dt295d: $(DAEMON_OBJS) $(SHARED_OBJS) $(PLATFORM_OBJS) $(UPSTREAM_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(DEPS_LIBS) -o $@

# 自動試験 (USB と CUSE を使わない)
$(BUILD)/control_channel_test: $(ROOT)/winusb/tests/dt295_control_channel_test.cpp $(SHARED_OBJS) $(PLATFORM_OBJS) $(UPSTREAM_OBJS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD)/channel_test: tests/channel_test.cpp src/channel.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

$(BUILD)/platform_test: tests/platform_test.cpp $(PLATFORM_OBJS) $(BUILD)/shared/source_firmware.o $(BUILD)/shared/frontend_init.o $(BUILD)/log.o $(UPSTREAM_OBJS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

check: $(BUILD)/control_channel_test $(BUILD)/channel_test $(BUILD)/platform_test
	$(BUILD)/control_channel_test
	$(BUILD)/channel_test
	$(BUILD)/platform_test

install: $(BUILD)/px4-dt295d
	install -D -m 0755 $(BUILD)/px4-dt295d $(DESTDIR)$(SBINDIR)/px4-dt295d
	install -D -m 0644 dist/px4-dt295d.service $(DESTDIR)$(UNITDIR)/px4-dt295d.service
	install -D -m 0644 dist/90-px4-dt295.rules $(DESTDIR)$(UDEVDIR)/90-px4-dt295.rules

clean:
	rm -rf $(BUILD)

.PHONY: all check install clean
