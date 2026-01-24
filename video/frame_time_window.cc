/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "video/frame_time_window.h"

#include <algorithm>
#include <map>

#include "rtc_base/logging.h"
#include "video/twcc_time_correlator.h"

namespace webrtc {

FrameTimeWindow::FrameTimeWindow(size_t window_size)
    : window_size_(window_size) {
}

FrameTimeWindow::~FrameTimeWindow() {
}

void FrameTimeWindow::AddFrame(uint32_t rtp_timestamp,
                               int64_t encode_start_time_ms,
                               int64_t encode_end_time_ms) {
  FrameTimingInfo info;
  info.rtp_timestamp = rtp_timestamp;
  info.encode_start_time_ms = encode_start_time_ms;
  info.encode_end_time_ms = encode_end_time_ms;
  frame_window_.push_back(info);
  MaintainWindowSize();
}

void FrameTimeWindow::AddMediaPacket(uint32_t rtp_timestamp,
                                     uint16_t rtp_seq,
                                     uint16_t transport_seq,
                                     int64_t send_time_ms) {
  FrameTimingInfo* info = FindFrameByTimestamp(rtp_timestamp);
  if (info) {
    info->media_packets.push_back(
        PacketTimingInfo{rtp_seq, transport_seq, send_time_ms});
  }
}

void FrameTimeWindow::AddRetransPacket(uint32_t rtp_timestamp,
                                       uint16_t rtp_seq,
                                       uint16_t transport_seq,
                                       int64_t send_time_ms) {
  FrameTimingInfo* info = FindFrameByTimestamp(rtp_timestamp);
  if (info) {
    info->retrans_packets.push_back(
        PacketTimingInfo{rtp_seq, transport_seq, send_time_ms});
  }
}

void FrameTimeWindow::PrintProfilingInfo(const std::vector<DecodeDelayInfo>& decode_delays,
                                         TwccTimeCorrelator* twcc_correlator) {
  RTC_LOG(LS_INFO) << "=== Profiling Report ===";
 
  for (const auto& decode_info : decode_delays) {
    FrameTimingInfo* frame_info = FindFrameByTimestamp(decode_info.rtp_timestamp);
    if (!frame_info) {
      continue;
    }

    std::map<uint16_t, PacketTimeInfo> twcc_packet_times;
    if (twcc_correlator) {
      for (const auto& packet_time :
           twcc_correlator->GetPacketTimesForFrame(decode_info.rtp_timestamp)) {
        twcc_packet_times[packet_time.sequence_number] = packet_time;
      }
    }
    
    RTC_LOG(LS_INFO) << "Frame RTP timestamp: " << decode_info.rtp_timestamp;
    RTC_LOG(LS_INFO) << "  Encode Start: " << frame_info->encode_start_time_ms
                     << " ms";
    RTC_LOG(LS_INFO) << "  Encode End: " << frame_info->encode_end_time_ms
                     << " ms";

    int64_t encode_time_ms =
        frame_info->encode_end_time_ms - frame_info->encode_start_time_ms;
    int64_t first_send_time_ms = -1;
    int64_t last_recv_time_ms = -1;
    for (const auto& packet_info : frame_info->media_packets) {
      if (first_send_time_ms == -1 ||
          packet_info.send_time_ms < first_send_time_ms) {
        first_send_time_ms = packet_info.send_time_ms;
      }
      auto it = twcc_packet_times.find(packet_info.transport_sequence_number);
      if (it != twcc_packet_times.end()) {
        int64_t recv_time_ms = it->second.receive_time_us / 1000;
        if (last_recv_time_ms == -1 || recv_time_ms > last_recv_time_ms) {
          last_recv_time_ms = recv_time_ms;
        }
      }
    }
    if (first_send_time_ms != -1) {
      RTC_LOG(LS_INFO) << "  Encode->FirstSend: "
                       << first_send_time_ms - frame_info->encode_end_time_ms
                       << " ms";
    } else {
      RTC_LOG(LS_INFO) << "  Encode->FirstSend: <n/a>";
    }
    RTC_LOG(LS_INFO) << "  LastRecv->Decode: "
                     << decode_info.assemble_to_decode_us / 1000 << " ms";
    RTC_LOG(LS_INFO) << "  Encode Time: " << encode_time_ms << " ms";
    RTC_LOG(LS_INFO) << "  Decode Time: " << decode_info.decode_delay_us / 1000
                     << " ms";

    int64_t encode_to_first_send_ms =
        first_send_time_ms != -1
            ? first_send_time_ms - frame_info->encode_end_time_ms
            : -1;
    int64_t last_recv_to_decode_ms = decode_info.assemble_to_decode_us / 1000;
    int64_t decode_time_ms = decode_info.decode_delay_us / 1000;
    RTC_LOG(LS_INFO) << "PRFL_FRAME rtp_ts=" << decode_info.rtp_timestamp
                     << " enc_start_ms=" << frame_info->encode_start_time_ms
                     << " enc_end_ms=" << frame_info->encode_end_time_ms
                     << " enc_time_ms=" << encode_time_ms
                     << " first_send_ms=" << first_send_time_ms
                     << " enc_to_first_send_ms=" << encode_to_first_send_ms
                     << " last_recv_ms=" << last_recv_time_ms
                     << " last_recv_to_decode_ms=" << last_recv_to_decode_ms
                     << " decode_time_ms=" << decode_time_ms;
    
    RTC_LOG(LS_INFO) << "  Media Packets:";
    for (const auto& packet_info : frame_info->media_packets) {
      auto it = twcc_packet_times.find(packet_info.transport_sequence_number);
      if (it != twcc_packet_times.end()) {
        int64_t recv_time_ms = it->second.receive_time_us / 1000;
        int64_t one_way_delay_ms = recv_time_ms - packet_info.send_time_ms;
        RTC_LOG(LS_INFO) << "    RTP Seq: " << packet_info.rtp_sequence_number
                         << ", Transport Seq: " << packet_info.transport_sequence_number
                         << ", Send: " << packet_info.send_time_ms << " ms"
                         << ", Recv: " << recv_time_ms << " ms"
                         << ", Delay: " << one_way_delay_ms << " ms";
        RTC_LOG(LS_INFO) << "PRFL_PKT rtp_ts=" << decode_info.rtp_timestamp
                         << " kind=media"
                         << " rtp_seq=" << packet_info.rtp_sequence_number
                         << " transport_seq="
                         << packet_info.transport_sequence_number
                         << " send_ms=" << packet_info.send_time_ms
                         << " recv_ms=" << recv_time_ms
                         << " delay_ms=" << one_way_delay_ms;
      } else {
        RTC_LOG(LS_INFO) << "    RTP Seq: " << packet_info.rtp_sequence_number
                         << ", Transport Seq: " << packet_info.transport_sequence_number
                         << ", Send: " << packet_info.send_time_ms << " ms"
                         << ", Recv: <n/a>";
        RTC_LOG(LS_INFO) << "PRFL_PKT rtp_ts=" << decode_info.rtp_timestamp
                         << " kind=media"
                         << " rtp_seq=" << packet_info.rtp_sequence_number
                         << " transport_seq="
                         << packet_info.transport_sequence_number
                         << " send_ms=" << packet_info.send_time_ms
                         << " recv_ms=-1"
                         << " delay_ms=-1";
      }
    }
    
    if (!frame_info->retrans_packets.empty()) {
      RTC_LOG(LS_INFO) << "  Retrans Packets:";
      for (const auto& packet_info : frame_info->retrans_packets) {
        auto it = twcc_packet_times.find(packet_info.transport_sequence_number);
        if (it != twcc_packet_times.end()) {
          int64_t recv_time_ms = it->second.receive_time_us / 1000;
          int64_t one_way_delay_ms = recv_time_ms - packet_info.send_time_ms;
          RTC_LOG(LS_INFO) << "    RTP Seq: " << packet_info.rtp_sequence_number
                           << ", Transport Seq: " << packet_info.transport_sequence_number
                           << ", Send: " << packet_info.send_time_ms << " ms"
                           << ", Recv: " << recv_time_ms << " ms"
                           << ", Delay: " << one_way_delay_ms << " ms";
          RTC_LOG(LS_INFO) << "PRFL_PKT rtp_ts=" << decode_info.rtp_timestamp
                           << " kind=retrans"
                           << " rtp_seq=" << packet_info.rtp_sequence_number
                           << " transport_seq="
                           << packet_info.transport_sequence_number
                           << " send_ms=" << packet_info.send_time_ms
                           << " recv_ms=" << recv_time_ms
                           << " delay_ms=" << one_way_delay_ms;
        } else {
          RTC_LOG(LS_INFO) << "    RTP Seq: " << packet_info.rtp_sequence_number
                           << ", Transport Seq: " << packet_info.transport_sequence_number
                           << ", Send: " << packet_info.send_time_ms << " ms"
                           << ", Recv: <n/a>";
          RTC_LOG(LS_INFO) << "PRFL_PKT rtp_ts=" << decode_info.rtp_timestamp
                           << " kind=retrans"
                           << " rtp_seq=" << packet_info.rtp_sequence_number
                           << " transport_seq="
                           << packet_info.transport_sequence_number
                           << " send_ms=" << packet_info.send_time_ms
                           << " recv_ms=-1"
                           << " delay_ms=-1";
        }
      }
    }
  }
}

FrameTimingInfo* FrameTimeWindow::FindFrameByTimestamp(uint32_t rtp_timestamp) {
  for (auto& info : frame_window_) {
    if (info.rtp_timestamp == rtp_timestamp) {
      return &info;
    }
  }
  return nullptr;
}

void FrameTimeWindow::MaintainWindowSize() {
  while (frame_window_.size() > window_size_) {
    frame_window_.pop_front();
  }
}

}  // namespace webrtc
