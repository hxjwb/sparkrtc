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

#include <string>
#include <utility>

#include "rtc_base/logging.h"
#include "video/twcc_time_correlator.h"

namespace webrtc {

FrameTimeWindow::FrameTimeWindow(size_t window_size)
    : window_size_(window_size) {
}

FrameTimeWindow::~FrameTimeWindow() {
}

void FrameTimeWindow::AddFrame(uint32_t rtp_timestamp,
                               int64_t capture_time_ms,
                               int64_t encode_start_time_ms,
                               int64_t encode_end_time_ms,
                               size_t frame_size_bytes,
                               bool is_keyframe) {
  FrameTimingInfo info;
  info.rtp_timestamp = rtp_timestamp;
  info.capture_time_ms = capture_time_ms;
  info.encode_start_time_ms = encode_start_time_ms;
  info.encode_end_time_ms = encode_end_time_ms;
  info.frame_size_bytes = frame_size_bytes;
  info.is_keyframe = is_keyframe;
  frame_window_.push_back(info);
  MaintainWindowSize();
}

void FrameTimeWindow::AddMediaPacket(uint32_t rtp_timestamp,
                                     uint16_t rtp_seq,
                                     uint16_t transport_seq,
                                     int64_t send_time_ms,
                                     size_t size_bytes) {
  FrameTimingInfo* info = FindFrameByTimestamp(rtp_timestamp);
  if (info) {
    info->media_packets.push_back(
        PacketTimingInfo{rtp_seq, transport_seq, send_time_ms, size_bytes,
                         PacketKind::kMedia, absl::nullopt, absl::nullopt});
  }
}

void FrameTimeWindow::AddRetransPacket(uint32_t rtp_timestamp,
                                       uint16_t rtp_seq,
                                       uint16_t transport_seq,
                                       int64_t send_time_ms,
                                       size_t size_bytes,
                                       absl::optional<uint16_t> rtx_target_seq) {
  PacketTimingInfo packet{rtp_seq, transport_seq, send_time_ms, size_bytes,
                          PacketKind::kRtx, rtx_target_seq, absl::nullopt};
  FrameTimingInfo* info = FindFrameByTimestamp(rtp_timestamp);
  if (info) {
    info->retrans_packets.push_back(packet);
  }
  rtx_packet_window_.push_back(packet);
  MaintainWindowSize();
}

void FrameTimeWindow::AddFecPacket(
    uint16_t rtp_seq,
    uint16_t transport_seq,
    int64_t send_time_ms,
    size_t size_bytes,
    const std::vector<uint16_t>& protected_sequence_numbers) {
  const uint32_t repair_id = AllocateRepairId();
  PacketTimingInfo packet_info{rtp_seq, transport_seq, send_time_ms, size_bytes,
                               PacketKind::kFec, absl::nullopt, repair_id};
  FecPacketInfo info;
  info.repair_id = repair_id;
  info.packet = packet_info;
  info.protected_sequence_numbers = protected_sequence_numbers;
  fec_packet_window_.push_back(std::move(info));
  MaintainWindowSize();
}

void FrameTimeWindow::SetCodecName(const std::string& codec) {
  report_header_.codec = codec;
}

void FrameTimeWindow::SetVideoDimensions(int width, int height) {
  report_header_.width = width;
  report_header_.height = height;
}

void FrameTimeWindow::SetFpsNominal(int fps) {
  report_header_.fps_nominal = fps;
}

void FrameTimeWindow::SetRtxFecEnabled(bool rtx_enabled, bool fec_enabled) {
  report_header_.rtx_enabled = rtx_enabled;
  report_header_.fec_enabled = fec_enabled;
}

namespace {
bool IsRtcpCounterEmpty(const RtcpPacketTypeCounter& counter);
constexpr int64_t kMinRtcpDigestIntervalMs = 50;
}  // namespace

void FrameTimeWindow::AddEncoderTargetRate(int64_t time_ms,
                                           uint32_t bitrate_bps) {
  if (last_encoder_target_bps_.has_value() &&
      *last_encoder_target_bps_ == bitrate_bps) {
    return;
  }
  last_encoder_target_bps_ = bitrate_bps;
  ControlDigestEntry entry;
  entry.time_ms = time_ms;
  entry.type = ControlDigestEntry::Type::kEncoderTargetRate;
  entry.bitrate_bps = bitrate_bps;
  control_digests_.push_back(std::move(entry));
  MaintainWindowSize();
}

void FrameTimeWindow::AddBweTargetRate(int64_t time_ms, uint32_t bitrate_bps) {
  if (last_bwe_target_bps_.has_value() &&
      *last_bwe_target_bps_ == bitrate_bps) {
    return;
  }
  last_bwe_target_bps_ = bitrate_bps;
  ControlDigestEntry entry;
  entry.time_ms = time_ms;
  entry.type = ControlDigestEntry::Type::kBweTargetRate;
  entry.bitrate_bps = bitrate_bps;
  control_digests_.push_back(std::move(entry));
  MaintainWindowSize();
}

void FrameTimeWindow::AddRtcpPacketTypeCounter(
    int64_t time_ms,
    const RtcpPacketTypeCounter& counter) {
  if (IsRtcpCounterEmpty(counter)) {
    return;
  }
  if (!control_digests_.empty()) {
    ControlDigestEntry& last = control_digests_.back();
    if (last.type == ControlDigestEntry::Type::kRtcpSignal &&
        last.time_ms == time_ms) {
      last.rtcp_counter = counter;
      last_rtcp_digest_ms_ = time_ms;
      return;
    }
  }
  if (last_rtcp_digest_ms_ >= 0 &&
      (time_ms - last_rtcp_digest_ms_) < kMinRtcpDigestIntervalMs) {
    return;
  }
  ControlDigestEntry entry;
  entry.time_ms = time_ms;
  entry.type = ControlDigestEntry::Type::kRtcpSignal;
  entry.rtcp_counter = counter;
  control_digests_.push_back(std::move(entry));
  last_rtcp_digest_ms_ = time_ms;
  MaintainWindowSize();
}   

void FrameTimeWindow::AddRtcpFeedbackEvent(int64_t time_ms,
                                           absl::string_view kind) {
  ControlDigestEntry entry;
  entry.time_ms = time_ms;
  entry.type = ControlDigestEntry::Type::kRtcpFeedback;
  entry.feedback_kind = std::string(kind);
  control_digests_.push_back(std::move(entry));
  MaintainWindowSize();
}

uint32_t FrameTimeWindow::AllocateRepairId() {
  return next_repair_id_++;
}

namespace {

bool IsRtcpCounterEmpty(const RtcpPacketTypeCounter& counter) {
  return counter.nack_packets == 0 && counter.fir_packets == 0 &&
         counter.pli_packets == 0 && counter.nack_requests == 0 &&
         counter.unique_nack_requests == 0;
}

}  // namespace

void FrameTimeWindow::PrintProfilingInfo(
    const std::vector<DecodeDelayInfo>& decode_delays,
    uint32_t stall_rtp_timestamp,
    int64_t stall_gap_ms,
    uint32_t stall_report_size_bytes,
    uint32_t nack_sent,
    TwccTimeCorrelator* twcc_correlator) {
  RTC_LOG(LS_INFO) << "===== Profiling Report ===";

  RTC_LOG(LS_INFO) << "[event_digest]";
  RTC_LOG(LS_INFO) << "codec=" << report_header_.codec
                   << " resolution=" << report_header_.width << "x"
                   << report_header_.height
                   << " fps_nominal=" << report_header_.fps_nominal
                   << " rtx_enabled=" << (report_header_.rtx_enabled ? 1 : 0)
                   << " fec_enabled=" << (report_header_.fec_enabled ? 1 : 0);
  RTC_LOG(LS_INFO) << "stall_rtp_ts=" << stall_rtp_timestamp
                   << " stall_gap_ms=" << stall_gap_ms
                   << " stall_report_size_bytes=" << stall_report_size_bytes
                   << " nack_sent=" << nack_sent
                   << " frame_count_in_window=" << decode_delays.size();
  RTC_LOG(LS_INFO) << "";

  RTC_LOG(LS_INFO) << "[timeline_digest]";
  if (decode_delays.empty()) {
    RTC_LOG(LS_INFO) << "none";
  } else {
    auto recv_time_ms = [&](uint16_t transport_sequence_number) -> int64_t {
      if (!twcc_correlator) {
        return -1;
      }
      auto packet_time = twcc_correlator->GetPacketTime(transport_sequence_number);
      if (!packet_time.has_value()) {
        return -1;
      }
      return packet_time->receive_time_us / 1000;
    };

    for (const auto& d : decode_delays) {
      FrameTimingInfo* frame = FindFrameByTimestamp(d.rtp_timestamp);
      const int64_t decode_ms = d.decode_delay_us / 1000;
      const int64_t assemble_to_decode_ms = d.assemble_to_decode_us / 1000;
      const int64_t decode_end_ms_rx =
          (d.decode_end_time_us > 0) ? (d.decode_end_time_us / 1000) : -1;

      RTC_LOG(LS_INFO) << "rtp_ts=" << d.rtp_timestamp
                       << " keyframe=" << (frame ? (frame->is_keyframe ? 1 : 0) : -1)
                       << " frame_size_bytes=" << (frame ? frame->frame_size_bytes : 0)
                       << " capture_ms=" << (frame ? frame->capture_time_ms : -1)
                       << " enc_start_ms=" << (frame ? frame->encode_start_time_ms : -1)
                       << " enc_end_ms=" << (frame ? frame->encode_end_time_ms : -1)
                       << " assemble_to_decode_ms=" << assemble_to_decode_ms
                       << " decode_ms=" << decode_ms
                       << " decode_end_ms_rx=" << decode_end_ms_rx;

      if (!frame) {
        continue;
      }

      int64_t base_send_ms = -1;
      int64_t base_recv_ms = -1;
      auto consider_packet_for_base = [&](const PacketTimingInfo& packet) {
        if (packet.send_time_ms >= 0 &&
            (base_send_ms < 0 || packet.send_time_ms < base_send_ms)) {
          base_send_ms = packet.send_time_ms;
        }
        const int64_t recv_ms = recv_time_ms(packet.transport_sequence_number);
        if (recv_ms >= 0 && (base_recv_ms < 0 || recv_ms < base_recv_ms)) {
          base_recv_ms = recv_ms;
        }
      };
      for (const auto& p : frame->media_packets) {
        consider_packet_for_base(p);
      }
      for (const auto& p : frame->retrans_packets) {
        consider_packet_for_base(p);
      }

      auto print_packet = [&](const PacketTimingInfo& packet) {
        const int64_t recv_ms = recv_time_ms(packet.transport_sequence_number);
        const int64_t send_delta_ms =
            (base_send_ms >= 0 && packet.send_time_ms >= 0)
                ? (packet.send_time_ms - base_send_ms)
                : -1;
        const int64_t recv_delta_ms =
            (base_recv_ms >= 0 && recv_ms >= 0) ? (recv_ms - base_recv_ms) : -1;
        const char* kind = (packet.kind == PacketKind::kRtx) ? "RTX" : "MEDIA";
        RTC_LOG(LS_INFO) << "P: kind=" << kind
                         << " rtp_seq=" << packet.rtp_sequence_number
                        //  << " transport_seq=" << packet.transport_sequence_number
                         << " size=" << packet.size_bytes
                         << " send_delta_ms="
                         << (send_delta_ms >= 0 ? std::to_string(send_delta_ms)
                                                : "None")
                         << " recv_delta_ms="
                         << (recv_delta_ms >= 0 ? std::to_string(recv_delta_ms)
                                                : "None");
                        //  << " rtx_target_seq="
                        //  << (packet.rtx_target_seq.has_value()
                        //          ? std::to_string(*packet.rtx_target_seq)
                        //          : "None");
      };

      for (const auto& p : frame->media_packets) {
        print_packet(p);
      }
      for (const auto& p : frame->retrans_packets) {
        print_packet(p);
      }
    }
  }
  RTC_LOG(LS_INFO) << "";

  RTC_LOG(LS_INFO) << "[control_digest]";
  if (control_digests_.empty()) {
    RTC_LOG(LS_INFO) << "none";
    return;
  }
  for (const auto& c : control_digests_) {
    if (c.type == ControlDigestEntry::Type::kEncoderTargetRate) {
      RTC_LOG(LS_INFO) << c.time_ms << " encoder_target_bps=" << c.bitrate_bps;
      continue;
    }
    if (c.type == ControlDigestEntry::Type::kBweTargetRate) {
      RTC_LOG(LS_INFO) << c.time_ms << " bwe_target_bps=" << c.bitrate_bps;
      continue;
    }
    if (c.type == ControlDigestEntry::Type::kRtcpFeedback) {
      RTC_LOG(LS_INFO) << c.time_ms << " rtcp_feedback_kind=" << c.feedback_kind;
      continue;
    }
    RTC_LOG(LS_INFO) << c.time_ms << " rtcp"
                     << " nack_packets=" << c.rtcp_counter.nack_packets
                     << " pli_packets=" << c.rtcp_counter.pli_packets
                     << " fir_packets=" << c.rtcp_counter.fir_packets
                     << " nack_requests=" << c.rtcp_counter.nack_requests
                     << " unique_nack_requests="
                     << c.rtcp_counter.unique_nack_requests;
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
  const size_t max_fec_packets = window_size_ * 4;
  while (fec_packet_window_.size() > max_fec_packets) {
    fec_packet_window_.pop_front();
  }
  const size_t max_rtx_packets = window_size_ * 4;
  while (rtx_packet_window_.size() > max_rtx_packets) {
    rtx_packet_window_.pop_front();
  }
  const size_t max_control_digests = window_size_ * 4;
  while (control_digests_.size() > max_control_digests) {
    control_digests_.pop_front();
  }
}

}  // namespace webrtc
