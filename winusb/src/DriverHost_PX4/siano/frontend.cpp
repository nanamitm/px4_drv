// SPDX-License-Identifier: GPL-2.0-or-later
// Control flow adapted from Khronos31/siano-userland, commit 2f32c2a34665d6ccca39c9362eb786be78351347.
#include "frontend.hpp"
#include <algorithm>
#include <cerrno>

namespace px4::siano {
int Frontend::Request(std::uint16_t type, std::uint16_t response,
                      const std::vector<std::uint32_t>& words, std::vector<std::uint8_t>& payload, unsigned timeout_ms)
{
    std::vector<std::uint8_t> message(SMS_HEADER_SIZE+words.size()*4);
    const auto src=(type==MSG_SMS_ISDBT_TUNE_REQ || type==MSG_SMS_ADD_PID_FILTER_REQ ||
        type==MSG_SMS_GET_STATISTICS_REQ || type==MSG_SMS_GET_STATISTICS_EX_REQ ||
        type==MSG_IQ_STREAM_REQ) ? SMS_DVBT_BDA_CONTROL_MSG_ID : 0;
    sms_pack_header(message.data(),type,static_cast<std::uint8_t>(src),SMS_HIF_TASK,
        static_cast<std::uint16_t>(message.size()),0);
    for (std::size_t i=0;i<words.size();++i) sms_put_le32(message.data()+SMS_HEADER_SIZE+i*4,words[i]);
    return io_.Exchange(message,response,payload,timeout_ms);
}
int Frontend::VersionInfo()
{
    std::vector<std::uint8_t> p;
    int ret=Request(MSG_SMS_GET_VERSION_EX_REQ,MSG_SMS_GET_VERSION_EX_RES,{},p);
    if (ret) return ret;
    if (p.size()<12) return -EBADMSG;
    mode_=p[4]; protocols_=p[5]; version_=static_cast<std::uint16_t>((p[10]<<8)|p[11]);
    return 0;
}
int Frontend::Initialize(const std::vector<std::uint8_t>& firmware, bool force, bool isdbt)
{
    if (force) {
        io_.Reloading(true);
        const int ret=Reload(firmware,isdbt);
        io_.Reloading(false);
        return ret;
    }
    int ret=VersionInfo();
    if (ret) return ret;
    // 実機では稼働済み ISDB-T へ INIT_DEVICE を再送すると HIF が応答しなくなる。
    if (mode_==DEVICE_MODE_ISDBT_BDA) return 0;
    std::vector<std::uint8_t> p;
    if (!(protocols_&(1U<<DEVICE_MODE_ISDBT_BDA))) {
        if (firmware.empty()) return -ENOENT;
        sms_firmware_header h{};
        if (sms_parse_firmware_header(firmware.data(),firmware.size(),&h) || !h.length ||
            firmware.size()!=static_cast<std::size_t>(h.length)+12 ||
            firmware.size()>UINT32_MAX-h.start_address) return -EBADMSG;
        // 上流の wire length はヘッダー込みのファイル長。末尾12バイトはゼロで補う。
        std::vector<std::uint8_t> image(firmware.size(),0);
        std::copy_n(firmware.data()+12,h.length,image.data());
        std::uint32_t address=h.start_address;
        if (mode_!=255) {
            if (h.length<24) return -EBADMSG;
            ret=Request(MSG_SW_RELOAD_START_REQ,MSG_SW_RELOAD_START_RES,{},p);
            if (ret) return ret;
            address=sms_get_le32(image.data()+20);
            if (image.size()>UINT32_MAX-address) return -EBADMSG;
        }
        for (std::size_t offset=0;offset<image.size();) {
            const auto chunk=(std::min)(image.size()-offset,static_cast<std::size_t>(SMS_MAX_PAYLOAD_SIZE));
            std::vector<std::uint8_t> message(SMS_HEADER_SIZE+4+chunk);
            sms_pack_header(message.data(),MSG_SMS_DATA_DOWNLOAD_REQ,0,SMS_HIF_TASK,
                static_cast<std::uint16_t>(message.size()),0);
            sms_put_le32(message.data()+SMS_HEADER_SIZE,address);
            std::copy_n(image.data()+offset,chunk,message.data()+SMS_HEADER_SIZE+4);
            ret=io_.Exchange(message,MSG_SMS_DATA_DOWNLOAD_RES,p,10000);
            if (ret) return ret;
            offset+=chunk; address+=static_cast<std::uint32_t>(chunk);
        }
        ret=Request(MSG_SMS_DATA_VALIDITY_REQ,MSG_SMS_DATA_VALIDITY_RES,{h.start_address,h.length,0},p);
        if (ret) return ret;
        ret=mode_==255 ? Request(MSG_SMS_SWDOWNLOAD_TRIGGER_REQ,MSG_SMS_SWDOWNLOAD_TRIGGER_RES,
            {h.start_address,6,0x200,0,4},p) : Request(MSG_SW_RELOAD_EXEC_REQ,0,{},p);
        if (ret) return ret;
        if ((ret=io_.Delay(400)) || (ret=VersionInfo())) return ret;
        if (mode_==DEVICE_MODE_ISDBT_BDA) return 0;
    }
    if (version_<0x800) return -ENOSYS;
    ret=Request(MSG_SMS_INIT_DEVICE_REQ,MSG_SMS_INIT_DEVICE_RES,{DEVICE_MODE_ISDBT_BDA},p);
    if (ret) return ret;
    ret=VersionInfo();
    return ret ? ret : mode_==DEVICE_MODE_ISDBT_BDA ? 0 : -EIO;
}
// 稼働中の firmware を置き換える (J.83B 用と通常の firmware の切り替え)。
// 実機では、通常の firmware は SW_RELOAD_START に応答して読み込みを待つが、J.83B 用の firmware は
// 応答せずに ROM のブートローダー (モード 255) へ戻った。そのため応答を待たずに GET_VERSION で状態を確かめ、
// ROM へ戻っていればコールド起動、そうでなければ読み込み直しの手順で書く
int Frontend::Reload(const std::vector<std::uint8_t>& firmware, bool isdbt)
{
    sms_firmware_header h{};
    if (firmware.empty()) return -ENOENT;
    if (sms_parse_firmware_header(firmware.data(),firmware.size(),&h) || h.length<24 ||
        firmware.size()!=static_cast<std::size_t>(h.length)+12 ||
        firmware.size()>UINT32_MAX-h.start_address) return -EBADMSG;
    std::vector<std::uint8_t> image(firmware.size(),0);
    std::copy_n(firmware.data()+12,h.length,image.data());
    int ret=VersionInfo();
    if (ret) return ret;
    std::vector<std::uint8_t> p;
    if (mode_!=255) {
        if ((ret=Request(MSG_SW_RELOAD_START_REQ,0,{},p)) || (ret=io_.Delay(400)) || (ret=VersionInfo())) return ret;
    }
    const bool cold=mode_==255;
    std::uint32_t address=cold ? h.start_address : sms_get_le32(image.data()+20);
    if (image.size()>UINT32_MAX-address) return -EBADMSG;
    for (std::size_t offset=0;offset<image.size();) {
        const auto chunk=(std::min)(image.size()-offset,static_cast<std::size_t>(SMS_MAX_PAYLOAD_SIZE));
        std::vector<std::uint8_t> message(SMS_HEADER_SIZE+4+chunk);
        sms_pack_header(message.data(),MSG_SMS_DATA_DOWNLOAD_REQ,0,SMS_HIF_TASK,static_cast<std::uint16_t>(message.size()),0);
        sms_put_le32(message.data()+SMS_HEADER_SIZE,address);
        std::copy_n(image.data()+offset,chunk,message.data()+SMS_HEADER_SIZE+4);
        if ((ret=io_.Exchange(message,MSG_SMS_DATA_DOWNLOAD_RES,p,10000))) return ret;
        offset+=chunk; address+=static_cast<std::uint32_t>(chunk);
    }
    ret=Request(MSG_SMS_DATA_VALIDITY_REQ,MSG_SMS_DATA_VALIDITY_RES,{h.start_address,h.length,0},p);
    if (ret) return ret;
    ret=cold ? Request(MSG_SMS_SWDOWNLOAD_TRIGGER_REQ,MSG_SMS_SWDOWNLOAD_TRIGGER_RES,{h.start_address,6,0x200,0,4},p) :
        Request(MSG_SW_RELOAD_EXEC_REQ,0,{},p);
    if (ret) return ret;
    if ((ret=io_.Delay(400)) || (ret=VersionInfo())) return ret;
    if (mode_==DEVICE_MODE_ISDBT_BDA) return 0;
    if (mode_==255) return -EIO;
    if (version_<0x800) return -ENOSYS;
    // BonDriver_S1UD.dll と同じく、J.83B 用の firmware にも ISDB-T の INIT_DEVICE を送る。
    // この firmware は送った後も独自のモード (実機では 7) を返すため、モードは確かめない
    if ((ret=Request(MSG_SMS_INIT_DEVICE_REQ,MSG_SMS_INIT_DEVICE_RES,{DEVICE_MODE_ISDBT_BDA},p)) || (ret=VersionInfo())) return ret;
    return !isdbt || mode_==DEVICE_MODE_ISDBT_BDA ? 0 : -EIO;
}
int Frontend::Tune(std::uint32_t frequency_khz)
{
    if (frequency_khz<473000 || frequency_khz>767500) return -EINVAL;
    // ChSet の kHz 精度から日本の 1/7 MHz オフセットを復元する。
    std::uint32_t hz=frequency_khz*1000;
    if (frequency_khz%1000==143) hz-=143;
    else if (frequency_khz%1000==142) hz+=857;
    std::vector<std::uint8_t> p;
    int ret=Request(MSG_SMS_ISDBT_TUNE_REQ,MSG_SMS_ISDBT_TUNE_RES,{hz,BW_ISDBT_13SEG,12000000,0},p);
    if (ret) return ret;
    // 上流は tune ACK 自体の到着で完了し、ロックは別の統計応答で判定する。
    // catch-all 0x2000 はこの firmware が ACK を返さないため、送信完了のみを待つ。
    return Request(MSG_SMS_ADD_PID_FILTER_REQ,0,{0x2000},p);
}
int Frontend::SetIqStream(std::uint32_t frequency_hz, std::vector<std::uint8_t>* stats)
{
    // J.83B のケーブルの周波数 (90〜775 MHz) を含む、チューナーの受信範囲
    if (frequency_hz && (frequency_hz<90000000 || frequency_hz>775000000)) return -EINVAL;
    std::vector<std::uint8_t> p;
    // この要求自体が選局を兼ねるため、ISDB-T の選局要求は送らない (BonDriver_S1UD.dll と同じ)。
    // 通常の firmware はこの要求に応答しないため、期限を短くして J.83B 用の firmware でないことを早く返す
    const int ret=Request(MSG_IQ_STREAM_REQ,MSG_IQ_STREAM_RES,{frequency_hz},p,2000);
    if (!ret && stats) *stats=std::move(p);
    return ret;
}
int Frontend::RequestStats(std::vector<std::uint8_t>& payload)
{
    return version_>=0x800 ? Request(MSG_SMS_GET_STATISTICS_EX_REQ,MSG_SMS_GET_STATISTICS_EX_RES,{},payload,500) :
        Request(MSG_SMS_GET_STATISTICS_REQ,MSG_SMS_GET_STATISTICS_RES,{},payload,500);
}
int Frontend::DecodeStats(const std::vector<std::uint8_t>& p, bool& locked, std::int32_t& cnr) const
{
    locked=false; cnr=0;
    // Linux smsdvb: EX は先頭に result word、以降は sms_isdbt_stats[_ex]。
    const std::size_t base=version_>=0x800 ? 4 : 0;
    if (p.size()<base+24 || (base && sms_get_le32(p.data())!=0)) return -EBADMSG;
    locked=sms_get_le32(p.data()+base+12)!=0;
    if (sms_get_le32(p.data()+base)==0) return 0; // 旧 firmware の CNR は未提供。
    const auto snr=static_cast<std::int32_t>(sms_get_le32(p.data()+base+20));
    if (locked && snr>=0 && snr<=100) cnr=snr*1000;
    return 0;
}
int Frontend::DecodeSignal(const std::vector<std::uint8_t>& p, SignalStats& s) const
{
    s=SignalStats{};
    // Linux smsdvb の sms_isdbt_stats_ex。共通部の後に segment_number と tune_bw があり、
    // layer_info[3] は 76 バイト目から 48 バイトずつ並ぶ。旧形式は階層の位置が異なるため扱わない。
    constexpr std::size_t base=4, layers=base+76, stride=48;
    if (version_<0x800) return -ENOSYS;
    if (p.size()<layers+stride*3 || sms_get_le32(p.data())!=0) return -EBADMSG;
    const auto u=[&](std::size_t offset) {return sms_get_le32(p.data()+offset);};
    s.rf_locked=u(base+8)!=0; s.demod_locked=u(base+12)!=0;
    s.snr=static_cast<std::int32_t>(u(base+20)); s.rssi=static_cast<std::int32_t>(u(base+24));
    s.frequency=u(base+36); s.layers=u(base+64);
    for (std::size_t i=0;i<3;++i) {
        const auto o=layers+i*stride; auto& l=s.layer[i];
        l.ber=u(o+8); l.error_ts=u(o+28); l.total_ts=u(o+32); l.segments=u(o+40); l.tmcc_errors=u(o+44);
    }
    return 0;
}
}
