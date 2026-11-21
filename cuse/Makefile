# SPDX-License-Identifier: GPL-2.0-only
# px4-cused: PIX-DT295 と Sony torne CECH-ZD1J の CUSE デーモン。
# WinUSB 版と共有するコードは winusb/src/DriverHost_PX4 から参照する。
CC ?= cc
CXX ?= c++
PKG_CONFIG ?= pkg-config
BUILD ?= build
ROOT := ..
SHARED := $(ROOT)/winusb/src/DriverHost_PX4
DT295 := $(SHARED)/dt295
TORNE := $(SHARED)/torne
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
# torne の共有コード。DiB8096 の生成コードは coax の Go 実装から生成したもの (winusb/tools/torne_frontend)。
TORNE_OBJS := $(BUILD)/torne/session.o $(BUILD)/torne/frontend_native_client.o $(BUILD)/torne/dib8096_native.o \
	$(BUILD)/torne/mb86h47_bootstrap.o $(BUILD)/torne/mb86h47_i2c.o $(BUILD)/torne/mb86h47_stream.o
PLATFORM_OBJS := $(BUILD)/platform_linux.o $(BUILD)/firmware.o
DAEMON_OBJS := $(BUILD)/main.o $(BUILD)/device.o $(BUILD)/video_node.o $(BUILD)/card_node.o \
	$(BUILD)/dt295_device.o $(BUILD)/torne_device.o $(BUILD)/torne_port.o \
	$(BUILD)/cuse_session.o $(BUILD)/usb_transport.o $(BUILD)/log.o

all: $(BUILD)/px4-cused

$(BUILD) $(BUILD)/shared $(BUILD)/upstream $(BUILD)/torne $(BUILD)/torne-trace:
	mkdir -p $@

$(BUILD)/upstream/%.o: $(UPSTREAM)/core/%.c | $(BUILD)/upstream
	$(CC) $(CPPFLAGS) $(CFLAGS) -include $(DT295)/compat.h -c $< -o $@

$(BUILD)/shared/%.o: $(DT295)/%.cpp $(DT295)/*.hpp | $(BUILD)/shared
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# 生成コードは Go からの機械的な変換のため、符号の比較などの警告は WinUSB 版 (Level3) と同じく対象外にする。
$(BUILD)/torne/dib8096_native.o: $(TORNE)/dib8096_native.cpp $(TORNE)/dib8096_native_generated.inc $(TORNE)/*.hpp | $(BUILD)/torne
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -w -c $< -o $@

$(BUILD)/torne/%.o: $(TORNE)/%.cpp $(TORNE)/*.hpp | $(BUILD)/torne
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.cpp src/*.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(DEPS_CFLAGS) $(CXXFLAGS) -c $< -o $@

# 埋め込むファームウェアが変わったら作り直す
$(BUILD)/firmware.o: src/firmware.S $(FIRMWARE)/dt295-direct-fr.bin $(FIRMWARE)/dt295-source-fr.bin | $(BUILD)
	$(CC) -c -Wa,-I$(FIRMWARE) $< -o $@

$(BUILD)/px4-cused: $(DAEMON_OBJS) $(SHARED_OBJS) $(TORNE_OBJS) $(PLATFORM_OBJS) $(UPSTREAM_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(DEPS_LIBS) -o $@

# 自動試験 (USB と CUSE を使わない)
$(BUILD)/control_channel_test: $(ROOT)/winusb/tests/dt295_control_channel_test.cpp $(SHARED_OBJS) $(PLATFORM_OBJS) $(UPSTREAM_OBJS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD)/channel_test: tests/channel_test.cpp src/channel.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

$(BUILD)/platform_test: tests/platform_test.cpp $(PLATFORM_OBJS) $(BUILD)/shared/source_firmware.o $(BUILD)/shared/frontend_init.o $(BUILD)/log.o $(UPSTREAM_OBJS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

# torne の共有セッション (WinUSB 版と同じ試験)
$(BUILD)/torne_session_test: $(ROOT)/winusb/tests/torne_session_test.cpp $(TORNE_OBJS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

# DiB8096 の生成コードを、リポジトリにある Go の正解の記録 (winusb/tests/data/torne_native) と比べる。Go は不要。
$(BUILD)/torne_native_frontend_test: $(ROOT)/winusb/tests/torne_native_frontend_test.cpp $(BUILD)/torne/dib8096_native.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

check: $(BUILD)/control_channel_test $(BUILD)/channel_test $(BUILD)/platform_test $(BUILD)/torne_session_test \
		$(BUILD)/torne_native_frontend_test | $(BUILD)/torne-trace
	$(BUILD)/control_channel_test
	$(BUILD)/channel_test
	$(BUILD)/platform_test
	$(BUILD)/torne_session_test
	$(BUILD)/torne_native_frontend_test
	$(BUILD)/torne_native_frontend_test --golden $(ROOT)/winusb/tests/data/torne_native $(BUILD)/torne-trace

install: $(BUILD)/px4-cused
	install -D -m 0755 $(BUILD)/px4-cused $(DESTDIR)$(SBINDIR)/px4-cused
	install -D -m 0644 dist/px4-cused.service $(DESTDIR)$(UNITDIR)/px4-cused.service
	install -D -m 0644 dist/90-px4-cused.rules $(DESTDIR)$(UDEVDIR)/90-px4-cused.rules

clean:
	rm -rf $(BUILD)

.PHONY: all check install clean
