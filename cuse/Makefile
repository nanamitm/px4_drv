# SPDX-License-Identifier: GPL-2.0-only
# px4-cused: PIX-DT295、Sony torne CECH-ZD1J、I-O DATA GV-MVP/XZ3、PLEX PX-BCUD、PLEX PX-S1UD、RTL-SDR の CUSE デーモン。
# WinUSB 版と共有するコードは winusb/src/DriverHost_PX4 から参照する。
CC ?= cc
CXX ?= c++
PKG_CONFIG ?= pkg-config
BUILD ?= build
ROOT := ..
SHARED := $(ROOT)/winusb/src/DriverHost_PX4
DT295 := $(SHARED)/dt295
TORNE := $(SHARED)/torne
XZ3 := $(SHARED)/xz3
PXBCUD := $(SHARED)/pxbcud
SIANO := $(SHARED)/siano
RTLSDR := $(SHARED)/rtlsdr
# RTL-SDR Blog V1.4.0 の librtlsdr (GPL-2.0-or-later)。変更点は third_party/rtl-sdr-blog/README.md
LIBRTLSDR := third_party/rtl-sdr-blog
UPSTREAM := $(DT295)/upstream
FIRMWARE := $(ROOT)/winusb/pkg/DriverHost_PX4/firmware

PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
UNITDIR ?= /lib/systemd/system
UDEVDIR ?= /lib/udev/rules.d
# PX-S1UD の J.83B の helper と firmware、RTL-SDR の helper の置き場所 (make install-j83b、make install-rtlsdr)
LIBDIR ?= $(PREFIX)/lib/px4-cused

DEPS_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0 fuse3)
DEPS_LIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0 fuse3)
CPPFLAGS += -I$(ROOT)/include -I$(SHARED) -I$(DT295) -I$(UPSTREAM)/include -I$(ROOT)/winusb/src/common -Isrc -I$(LIBRTLSDR)/include
CPPFLAGS += -DPX4_CUSED_LIBDIR='"$(LIBDIR)"'
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
# GV-MVP/XZ3 の共有コード。xz3/card_uart.cpp は dt295/card_uart.cpp と名前が重なるため、出力先を分ける。
XZ3_OBJS := $(BUILD)/xz3/session.o $(BUILD)/xz3/frontend.o $(BUILD)/xz3/card_uart.o
# PX-BCUD の共有コード。xz3/frontend.cpp と名前が重なるため、出力先を分ける。
PXBCUD_OBJS := $(BUILD)/pxbcud/frontend.o
# PX-S1UD の共有コード。frontend.cpp と session.cpp はほかの機種と名前が重なるため、出力先を分ける。
SIANO_OBJS := $(BUILD)/siano/protocol.o $(BUILD)/siano/frontend.o $(BUILD)/siano/session.o
# RTL-SDR の共有コード (siano/session.cpp と名前が重なるため、出力先を分ける) と librtlsdr
RTLSDR_OBJS := $(BUILD)/rtlsdr/session.o
LIBRTLSDR_OBJS := $(BUILD)/librtlsdr/librtlsdr.o $(BUILD)/librtlsdr/tuner_e4k.o $(BUILD)/librtlsdr/tuner_fc0012.o 	$(BUILD)/librtlsdr/tuner_fc0013.o $(BUILD)/librtlsdr/tuner_fc2580.o $(BUILD)/librtlsdr/tuner_r82xx.o
PLATFORM_OBJS := $(BUILD)/platform_linux.o $(BUILD)/firmware.o
DAEMON_OBJS := $(BUILD)/main.o $(BUILD)/device.o $(BUILD)/video_node.o $(BUILD)/card_node.o \
	$(BUILD)/dt295_device.o $(BUILD)/torne_device.o $(BUILD)/torne_port.o $(BUILD)/xz3_device.o $(BUILD)/pxbcud_device.o \
	$(BUILD)/pxs1ud_device.o $(BUILD)/iq_process.o $(BUILD)/rtlsdr_device.o $(BUILD)/rtlsdr_helper.o \
	$(BUILD)/cuse_session.o $(BUILD)/usb_transport.o $(BUILD)/log.o

all: $(BUILD)/px4-cused

$(BUILD) $(BUILD)/shared $(BUILD)/upstream $(BUILD)/torne $(BUILD)/torne-trace $(BUILD)/xz3 $(BUILD)/pxbcud $(BUILD)/siano $(BUILD)/rtlsdr $(BUILD)/librtlsdr $(BUILD)/fault:
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

$(BUILD)/xz3/%.o: $(XZ3)/%.cpp $(XZ3)/*.hpp | $(BUILD)/xz3
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/pxbcud/%.o: $(PXBCUD)/%.cpp $(PXBCUD)/*.hpp | $(BUILD)/pxbcud
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/siano/%.o: $(SIANO)/%.cpp $(SIANO)/*.hpp $(SIANO)/*.h | $(BUILD)/siano
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/siano/%.o: $(SIANO)/%.c $(SIANO)/*.h | $(BUILD)/siano
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/rtlsdr/%.o: $(RTLSDR)/%.cpp $(RTLSDR)/*.hpp | $(BUILD)/rtlsdr
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# 上流のコードの警告は対象外にする (生成コードと同じく、上流との差分を変更点だけに保つため)。
# 機器を開く処理はデーモンが行うため、上流のビルドの DETACH_KERNEL_DRIVER は定義しない
$(BUILD)/librtlsdr/%.o: $(LIBRTLSDR)/src/%.c $(LIBRTLSDR)/include/*.h | $(BUILD)/librtlsdr
	$(CC) $(CPPFLAGS) $(DEPS_CFLAGS) $(CFLAGS) -Drtlsdr_STATIC -w -c $< -o $@

$(BUILD)/%.o: src/%.cpp src/*.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(DEPS_CFLAGS) $(CXXFLAGS) -c $< -o $@

# 埋め込むファームウェアが変わったら作り直す
$(BUILD)/firmware.o: src/firmware.S $(FIRMWARE)/dt295-direct-fr.bin $(FIRMWARE)/dt295-source-fr.bin \
		$(FIRMWARE)/xz3-bootstrap.bin $(FIRMWARE)/xz3-runtime.bin $(FIRMWARE)/xz3-hook.bin | $(BUILD)
	$(CC) -c -Wa,-I$(FIRMWARE) $< -o $@

$(BUILD)/px4-cused: $(DAEMON_OBJS) $(SHARED_OBJS) $(TORNE_OBJS) $(XZ3_OBJS) $(PXBCUD_OBJS) $(SIANO_OBJS) $(RTLSDR_OBJS) 		$(LIBRTLSDR_OBJS) $(PLATFORM_OBJS) $(UPSTREAM_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(DEPS_LIBS) -o $@

# PX-BCUD の自動復旧を模擬の故障で試すデーモン (make fault)。配布するデーモンへ故障の処理を含めないため、
# 別の実行ファイルとして作り、install の対象にしない。使い方は cuse/src/pxbcud_device.cpp の Fault を参照。
$(BUILD)/fault/pxbcud_device.o: src/pxbcud_device.cpp src/*.hpp | $(BUILD)/fault
	$(CXX) $(CPPFLAGS) $(DEPS_CFLAGS) $(CXXFLAGS) -DPX4_CUSED_FAULT_INJECTION -c $< -o $@

$(BUILD)/px4-cused-fault: $(filter-out $(BUILD)/pxbcud_device.o,$(DAEMON_OBJS)) $(BUILD)/fault/pxbcud_device.o \
		$(SHARED_OBJS) $(TORNE_OBJS) $(XZ3_OBJS) $(PXBCUD_OBJS) $(SIANO_OBJS) $(RTLSDR_OBJS) $(LIBRTLSDR_OBJS) 		$(PLATFORM_OBJS) $(UPSTREAM_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(DEPS_LIBS) -o $@

fault: $(BUILD)/px4-cused-fault

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

# GV-MVP/XZ3 の共有コード (WinUSB 版と同じ試験)
$(BUILD)/xz3_card_test: $(ROOT)/winusb/tests/xz3_card_test.cpp $(BUILD)/xz3/card_uart.o $(BUILD)/shared/card_uart.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD)/xz3_stream_test: $(ROOT)/winusb/tests/xz3_stream_test.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

# PX-BCUD の共有コード (WinUSB 版と同じ試験) と、isochronous のパケットから TS を組み立てる処理
$(BUILD)/pxbcud_frontend_test: $(ROOT)/winusb/tests/pxbcud_frontend_test.cpp $(PXBCUD_OBJS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD)/pxbcud_stream_test: tests/pxbcud_stream_test.cpp src/pxbcud_stream.hpp $(PXBCUD)/*.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

# PX-S1UD の共有コード (WinUSB 版と同じ試験) と、J.83B の helper の子プロセスの扱い (模擬の helper のシェルスクリプト)
$(BUILD)/siano_frontend_test: $(ROOT)/winusb/tests/siano_frontend_test.cpp $(BUILD)/siano/protocol.o $(BUILD)/siano/frontend.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD)/siano_session_test: $(ROOT)/winusb/tests/siano_session_test.cpp $(SIANO_OBJS) $(BUILD)/log.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD)/iq_process_test: tests/iq_process_test.cpp $(BUILD)/iq_process.o $(SIANO_OBJS) $(BUILD)/log.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

# RTL-SDR の共有コード (WinUSB 版と同じ試験) と、helper の子プロセスの扱い (模擬の helper のシェルスクリプト)
$(BUILD)/rtlsdr_protocol_test: $(ROOT)/winusb/tests/rtlsdr_protocol_test.cpp $(RTLSDR)/protocol.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

$(BUILD)/rtlsdr_session_test: $(ROOT)/winusb/tests/rtlsdr_session_test.cpp $(RTLSDR_OBJS) $(BUILD)/log.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD)/rtlsdr_helper_test: tests/rtlsdr_helper_test.cpp $(BUILD)/rtlsdr_helper.o $(RTLSDR_OBJS) $(BUILD)/log.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

# PX-S1UD の J.83B に使う helper (winusb/tools/msi2500_frontend の serve-s1ud、GPL-3.0-or-later) と firmware
# (fwtool/s1ud/capture_probe.S から作る s1ud_j83b.inp、GPL-2.0-only)。地デジだけを使う場合は不要で、
# Rust (cargo) と ARC の binutils (binutils-arc-linux-gnu) が必要なため、既定の all には含めない。
HELPER_DIR := $(ROOT)/winusb/tools/msi2500_frontend
$(BUILD)/cargo/release/msi2500_frontend: $(HELPER_DIR)/Cargo.toml $(HELPER_DIR)/Cargo.lock $(HELPER_DIR)/src/*.rs | $(BUILD)
	cargo build --release --locked --manifest-path $(HELPER_DIR)/Cargo.toml --target-dir $(BUILD)/cargo --bin msi2500_frontend

# build_probe.py は既存のファイルを上書きしないため、作り直す前に消す
$(BUILD)/s1ud_j83b.inp: $(ROOT)/fwtool/s1ud/capture_probe.S $(ROOT)/fwtool/s1ud/build_probe.py | $(BUILD)
	rm -f $@
	python3 $(ROOT)/fwtool/s1ud/build_probe.py --probe capture $@

j83b: $(BUILD)/cargo/release/msi2500_frontend $(BUILD)/s1ud_j83b.inp

install-j83b: j83b
	install -D -m 0755 $(BUILD)/cargo/release/msi2500_frontend $(DESTDIR)$(LIBDIR)/msi2500_frontend
	install -D -m 0644 $(BUILD)/s1ud_j83b.inp $(DESTDIR)$(LIBDIR)/s1ud_j83b.inp

# RTL-SDR のワンセグの helper (winusb/tools/rtlsdr_frontend の --stdin、GPL-3.0-or-later)。
# RTL-SDR を使わない場合は不要で、Rust (cargo) が必要なため、既定の all には含めない
RTLSDR_HELPER_DIR := $(ROOT)/winusb/tools/rtlsdr_frontend
$(BUILD)/cargo/release/rtlsdr_frontend: $(RTLSDR_HELPER_DIR)/Cargo.toml $(RTLSDR_HELPER_DIR)/Cargo.lock $(RTLSDR_HELPER_DIR)/src/*.rs 		$(RTLSDR_HELPER_DIR)/vendor/isdbt-dsp/src/*.rs | $(BUILD)
	cargo build --release --locked --manifest-path $(RTLSDR_HELPER_DIR)/Cargo.toml --target-dir $(BUILD)/cargo --bin rtlsdr_frontend

rtlsdr: $(BUILD)/cargo/release/rtlsdr_frontend

install-rtlsdr: rtlsdr
	install -D -m 0755 $(BUILD)/cargo/release/rtlsdr_frontend $(DESTDIR)$(LIBDIR)/rtlsdr_frontend

check: $(BUILD)/control_channel_test $(BUILD)/channel_test $(BUILD)/platform_test $(BUILD)/torne_session_test \
		$(BUILD)/torne_native_frontend_test $(BUILD)/xz3_card_test $(BUILD)/xz3_stream_test \
		$(BUILD)/pxbcud_frontend_test $(BUILD)/pxbcud_stream_test $(BUILD)/siano_frontend_test $(BUILD)/siano_session_test \
		$(BUILD)/iq_process_test $(BUILD)/rtlsdr_protocol_test $(BUILD)/rtlsdr_session_test $(BUILD)/rtlsdr_helper_test 		| $(BUILD)/torne-trace
	$(BUILD)/control_channel_test
	$(BUILD)/channel_test
	$(BUILD)/platform_test
	$(BUILD)/torne_session_test
	$(BUILD)/torne_native_frontend_test
	$(BUILD)/torne_native_frontend_test --golden $(ROOT)/winusb/tests/data/torne_native $(BUILD)/torne-trace
	$(BUILD)/xz3_card_test
	$(BUILD)/xz3_stream_test
	$(BUILD)/pxbcud_frontend_test
	$(BUILD)/pxbcud_stream_test
	$(BUILD)/siano_frontend_test
	$(BUILD)/siano_session_test
	install -m 0755 tests/fake_iq_helper.sh $(BUILD)/fake_iq_helper
	$(BUILD)/iq_process_test $(BUILD)/fake_iq_helper
	$(BUILD)/rtlsdr_protocol_test
	$(BUILD)/rtlsdr_session_test
	install -m 0755 tests/fake_rtlsdr_helper.sh $(BUILD)/fake_rtlsdr_helper
	$(BUILD)/rtlsdr_helper_test $(BUILD)/fake_rtlsdr_helper

install: $(BUILD)/px4-cused
	install -D -m 0755 $(BUILD)/px4-cused $(DESTDIR)$(SBINDIR)/px4-cused
	install -D -m 0644 dist/px4-cused.service $(DESTDIR)$(UNITDIR)/px4-cused.service
	install -D -m 0644 dist/90-px4-cused.rules $(DESTDIR)$(UDEVDIR)/90-px4-cused.rules

clean:
	rm -rf $(BUILD)

.PHONY: all check fault j83b rtlsdr install install-j83b install-rtlsdr clean
