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
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
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

int64_t Percentile(std::vector<int64_t> values, double pct) {
  if (values.empty()) {
    return -1;
  }
  std::sort(values.begin(), values.end());
  const double clamped = std::min(1.0, std::max(0.0, pct));
  const size_t idx =
      static_cast<size_t>(std::ceil(clamped * (values.size() - 1)));
  return values[idx];
}

int64_t RelativeTimeMs(int64_t time_ms, int64_t base_ms) {
  if (time_ms < 0) {
    return -1;
  }
  return time_ms - base_ms;
}


void UpdateMinTimeMs(int64_t time_ms, int64_t* min_time_ms) {
  if (time_ms < 0) {
    return;
  }
  if (*min_time_ms < 0 || time_ms < *min_time_ms) {
    *min_time_ms = time_ms;
  }
}

void UpdateMaxTimeMs(int64_t time_ms, int64_t* max_time_ms) {
  if (time_ms < 0) {
    return;
  }
  if (*max_time_ms < 0 || time_ms > *max_time_ms) {
    *max_time_ms = time_ms;
  }
}

bool IsRtcpCounterEmpty(const RtcpPacketTypeCounter& counter) {
  return counter.nack_packets == 0 && counter.fir_packets == 0 &&
         counter.pli_packets == 0 && counter.nack_requests == 0 &&
         counter.unique_nack_requests == 0;
}

std::string FormatBitrateLabel(uint32_t bitrate_bps) {
  std::ostringstream oss;
  if (bitrate_bps >= 1000000) {
    const double mbps = bitrate_bps / 1000000.0;
    oss << "~" << std::fixed << std::setprecision(mbps >= 10.0 ? 0 : 1)
        << mbps << "Mbps";
  } else {
    const double kbps = bitrate_bps / 1000.0;
    oss << "~" << std::fixed << std::setprecision(kbps >= 100.0 ? 0 : 1)
        << kbps << "Kbps";
  }
  return oss.str();
}

// std::string CompressRateSeries(
//     const std::vector<std::pair<int64_t, uint32_t>>& points) {
//   if (points.empty()) {
//     return "none";
//   }
//   std::vector<std::pair<int64_t, uint32_t>> sorted = points;
//   std::sort(sorted.begin(), sorted.end(),
//             [](const auto& a, const auto& b) { return a.first < b.first; });
//   std::vector<std::pair<int64_t, uint32_t>> deduped;
//   deduped.reserve(sorted.size());
//   for (const auto& entry : sorted) {
//     if (!deduped.empty() && deduped.back().first == entry.first) {
//       deduped.back().second = entry.second;
//     } else {
//       deduped.push_back(entry);
//     }
//   }
//   if (deduped.size() == 1) {
//     std::ostringstream single;
//     single << deduped.front().first << "ms steady "
//            << FormatBitrateLabel(deduped.front().second);
//     return single.str();
//   }

//   enum class Trend { kStable, kRise, kFall };
//   struct Segment {
//     int64_t start_ms;
//     int64_t end_ms;
//     uint32_t start_bps;
//     uint32_t end_bps;
//     Trend trend;
//   };
//   constexpr double kStablePct = 0.05;
//   constexpr uint32_t kStableAbsBps = 50000;
//   constexpr int64_t kMinSegmentMs = 120;
//   auto EvaluateTrend = [&](uint32_t start_bps, uint32_t end_bps) {
//     const double delta = static_cast<double>(end_bps) - start_bps;
//     const double stable_threshold =
//         std::max(kStableAbsBps, static_cast<uint32_t>(start_bps * kStablePct));
//     if (std::abs(delta) <= stable_threshold) {
//       return Trend::kStable;
//     }
//     return delta > 0 ? Trend::kRise : Trend::kFall;
//   };
//   std::vector<Segment> segments;
//   for (size_t i = 0; i + 1 < deduped.size(); ++i) {
//     const int64_t start_ms = deduped[i].first;
//     const int64_t end_ms = deduped[i + 1].first;
//     if (end_ms <= start_ms) {
//       continue;
//     }
//     const uint32_t start_bps = deduped[i].second;
//     const uint32_t end_bps = deduped[i + 1].second;
//     Trend trend = EvaluateTrend(start_bps, end_bps);
//     Segment current{start_ms, end_ms, start_bps, end_bps, trend};
//     segments.push_back(current);
//   }
//   if (segments.empty()) {
//     return "none";
//   }
//   std::vector<Segment> merged;
//   merged.reserve(segments.size());
//   for (const auto& seg : segments) {
//     const int64_t duration = seg.end_ms - seg.start_ms;
//     if (duration < kMinSegmentMs && !merged.empty()) {
//       Segment& prev = merged.back();
//       prev.end_ms = seg.end_ms;
//       prev.end_bps = seg.end_bps;
//       prev.trend = EvaluateTrend(prev.start_bps, prev.end_bps);
//       continue;
//     }
//     merged.push_back(seg);
//   }
//   std::vector<Segment> compacted;
//   compacted.reserve(merged.size());
//   for (const auto& seg : merged) {
//     if (compacted.empty()) {
//       compacted.push_back(seg);
//       continue;
//     }
//     Segment& prev = compacted.back();
//     if (prev.trend == seg.trend && prev.end_ms == seg.start_ms) {
//       prev.end_ms = seg.end_ms;
//       prev.end_bps = seg.end_bps;
//       prev.trend = EvaluateTrend(prev.start_bps, prev.end_bps);
//       continue;
//     }
//     compacted.push_back(seg);
//   }
//   if (compacted.empty()) {
//     return "none";
//   }
//   std::ostringstream oss;
//   for (size_t i = 0; i < compacted.size(); ++i) {
//     const Segment& seg = compacted[i];
//     if (i > 0) {
//       oss << "; ";
//     }
//     const std::string start_label = FormatBitrateLabel(seg.start_bps);
//     const std::string end_label = FormatBitrateLabel(seg.end_bps);
//     if (seg.trend == Trend::kStable) {
//       oss << seg.start_ms << "-" << seg.end_ms << "ms stable " << start_label;
//       if (seg.end_bps != seg.start_bps) {
//         oss << "->" << end_label;
//       }
//     } else if (seg.trend == Trend::kRise) {
//       oss << seg.start_ms << "-" << seg.end_ms << "ms rise " << start_label
//           << "->" << end_label;
//     } else {
//       oss << seg.start_ms << "-" << seg.end_ms << "ms drop " << start_label
//           << "->" << end_label;
//     }
//   }
//   return oss.str();
// }

const char* PacketKindToString(PacketKind kind) {
  switch (kind) {
    case PacketKind::kMedia:
      return "MEDIA";
    case PacketKind::kRtx:
      return "RTX";
    case PacketKind::kFec:
      return "FEC";
  }
  return "UNKNOWN";
}

struct PacketDigestEntry {
  uint32_t rtp_timestamp = 0;
  uint16_t rtp_sequence_number = 0;
  uint16_t transport_sequence_number = 0;
  int64_t send_time_ms = -1;
  int64_t recv_time_ms = -1;
  size_t size_bytes = 0;
  PacketKind kind = PacketKind::kMedia;
  absl::optional<uint16_t> rtx_target_seq;
  absl::optional<uint32_t> fec_repair_id;
};

struct RepairDigestEntry {
  uint32_t repair_id = 0;
  const char* type = "RTX";
  absl::optional<uint32_t> target_frame_ts;
  std::string target_frames_summary;
  std::vector<uint16_t> target_seqs;
  std::vector<uint16_t> repair_packet_seqs;
  int repair_packet_count = 0;
  int target_seq_count = 0;
  int64_t nack_request_ms = -1;
  int64_t repair_first_send_ms = -1;
  int64_t repair_last_send_ms = -1;
  int64_t repair_first_recv_ms = -1;
  int64_t repair_last_recv_ms = -1;
  int64_t duration_ms = -1;
  const char* outcome = "FAILED";
  std::vector<uint16_t> recovered_seqs;
  std::vector<uint16_t> too_late_seqs;
  std::vector<uint16_t> failed_seqs;
  int recovered_seq_count = 0;
  int too_late_seq_count = 0;
  int failed_seq_count = 0;
  std::string frame_outcome_summary;
};

struct FrameDigestEntry {
  uint32_t rtp_timestamp = 0;
  bool is_keyframe = false;
  size_t frame_size_bytes = 0;
  int packets_expected = -1;
  int64_t capture_time_ms = -1;
  int64_t enc_start_ms = -1;
  int64_t enc_end_ms = -1;
  int64_t first_send_ms = -1;
  int64_t last_send_ms = -1;
  int64_t first_recv_ms = -1;
  int64_t last_recv_ms = -1;
  int64_t deliver_to_decoder_ms = -1;
  int64_t decode_end_ms = -1;
  int64_t decode_time_ms = -1;
  int64_t decode_end_time_ms_rx = -1;
  int packets_received = -1;
  int packets_lost = -1;
  int packets_recovered_rtx = 0;
  int packets_recovered_fec = 0;
};

}  // namespace

void FrameTimeWindow::PrintProfilingInfo(
    const std::vector<DecodeDelayInfo>& decode_delays,
    uint32_t stall_rtp_timestamp,
    int64_t stall_gap_ms,
    uint32_t stall_report_size_bytes,
    uint32_t nack_sent,
    TwccTimeCorrelator* twcc_correlator) {
  RTC_LOG(LS_INFO) << "===== Profiling Report ===";
  RTC_LOG(LS_INFO) << "PRFL_REPORT codec=" << report_header_.codec
                   << " width=" << report_header_.width
                   << " height=" << report_header_.height
                   << " fps_nominal=" << report_header_.fps_nominal
                   << " rtx_enabled=" << (report_header_.rtx_enabled ? 1 : 0)
                   << " fec_enabled=" << (report_header_.fec_enabled ? 1 : 0)
                   << " stall_rtp_ts=" << stall_rtp_timestamp
                   << " stall_gap_ms=" << stall_gap_ms;

  std::unordered_map<uint16_t, uint32_t> seq_to_frame;
  std::set<uint32_t> window_frames;
  for (const auto& decode_info : decode_delays) {
    window_frames.insert(decode_info.rtp_timestamp);
    FrameTimingInfo* frame_info = FindFrameByTimestamp(decode_info.rtp_timestamp);
    if (!frame_info) {
      continue;
    }
    for (const auto& packet_info : frame_info->media_packets) {
      seq_to_frame[packet_info.rtp_sequence_number] = frame_info->rtp_timestamp;
    }
  }

  std::vector<PacketTimingInfo> rtx_packets_all;
  std::unordered_map<uint32_t, std::vector<PacketTimingInfo>> rtx_by_frame;
  std::unordered_map<uint16_t, std::vector<PacketTimingInfo>> rtx_by_target;
  std::unordered_set<uint32_t> rtx_seen;
  auto add_rtx_packet = [&](const PacketTimingInfo& packet) {
    if (!packet.rtx_target_seq.has_value()) {
      return;
    }
    const uint16_t target_seq = *packet.rtx_target_seq;
    auto frame_it = seq_to_frame.find(target_seq);
    if (frame_it == seq_to_frame.end()) {
      return;
    }
    if (window_frames.count(frame_it->second) == 0) {
      return;
    }
    const uint32_t key =
        (static_cast<uint32_t>(packet.rtp_sequence_number) << 16) |
        packet.transport_sequence_number;
    if (!rtx_seen.insert(key).second) {
      return;
    }
    rtx_packets_all.push_back(packet);
    rtx_by_target[target_seq].push_back(packet);
    rtx_by_frame[frame_it->second].push_back(packet);
  };
  for (const auto& decode_info : decode_delays) {
    FrameTimingInfo* frame_info =
        FindFrameByTimestamp(decode_info.rtp_timestamp);
    if (!frame_info) {
      continue;
    }
    for (const auto& packet_info : frame_info->retrans_packets) {
      add_rtx_packet(packet_info);
    }
  }
  for (const auto& packet_info : rtx_packet_window_) {
    add_rtx_packet(packet_info);
  }

  std::unordered_map<uint32_t, int64_t> deliver_to_decoder_ms;
  std::unordered_map<uint32_t, int64_t> decode_end_ms;

  std::vector<int64_t> send_gaps;
  std::vector<int64_t> net_gaps;
  std::vector<int64_t> recv_gaps;
  std::vector<int64_t> dec_gaps;
  std::vector<int64_t> one_way_delays;
  int64_t max_frame_e2e_ms = -1;
  uint32_t loss_packets = 0;
  uint32_t rtx_recv = 0;
  uint32_t fec_recv = 0;

  std::vector<FrameDigestEntry> frame_digests;
  std::vector<PacketDigestEntry> packet_digests;
  std::vector<RepairDigestEntry> repair_digests;

  const std::vector<PacketTimingInfo> empty_rtx_packets;
  for (const auto& decode_info : decode_delays) {
    FrameTimingInfo* frame_info = FindFrameByTimestamp(decode_info.rtp_timestamp);
    const int64_t decode_time_ms = decode_info.decode_delay_us / 1000;
    const int64_t last_recv_to_decode_ms = decode_info.assemble_to_decode_us / 1000;

    int64_t capture_time_ms = -1;
    int64_t enc_start_ms = -1;
    int64_t enc_end_ms = -1;
    size_t frame_size_bytes = 0;
    bool is_keyframe = false;
    std::vector<PacketTimingInfo> media_packets;
    if (frame_info) {
      capture_time_ms = frame_info->capture_time_ms;
      enc_start_ms = frame_info->encode_start_time_ms;
      enc_end_ms = frame_info->encode_end_time_ms;
      frame_size_bytes = frame_info->frame_size_bytes;
      is_keyframe = frame_info->is_keyframe;
      media_packets = frame_info->media_packets;
    }
    const auto rtx_it = rtx_by_frame.find(decode_info.rtp_timestamp);
    const std::vector<PacketTimingInfo>& frame_rtx_packets =
        (rtx_it != rtx_by_frame.end()) ? rtx_it->second : empty_rtx_packets;

    int64_t first_send_ms = -1;
    int64_t last_send_ms = -1;
    int64_t first_recv_ms = -1;
    int64_t last_recv_ms = -1;
    int packets_received = -1;
    int packets_lost = -1;
    int twcc_received = 0;
    auto update_recv_time_ms = [&](int64_t recv_time_ms) {
      if (first_recv_ms == -1 || recv_time_ms < first_recv_ms) {
        first_recv_ms = recv_time_ms;
      }
      if (last_recv_ms == -1 || recv_time_ms > last_recv_ms) {
        last_recv_ms = recv_time_ms;
      }
    };
    for (const auto& packet_info : media_packets) {
      if (first_send_ms == -1 || packet_info.send_time_ms < first_send_ms) {
        first_send_ms = packet_info.send_time_ms;
      }
      if (last_send_ms == -1 || packet_info.send_time_ms > last_send_ms) {
        last_send_ms = packet_info.send_time_ms;
      }
      if (twcc_correlator) {
        auto packet_time =
            twcc_correlator->GetPacketTime(
                packet_info.transport_sequence_number);
        if (packet_time.has_value()) {
          update_recv_time_ms(packet_time->receive_time_us / 1000);
          ++twcc_received;
        }
      }
    }
    for (const auto& packet_info : frame_rtx_packets) {
      if (!twcc_correlator) {
        continue;
      }
      auto packet_time =
          twcc_correlator->GetPacketTime(
              packet_info.transport_sequence_number);
      if (!packet_time.has_value()) {
        continue;
      }
      update_recv_time_ms(packet_time->receive_time_us / 1000);
    }

    const int packets_expected = static_cast<int>(media_packets.size());
    if (twcc_received > 0) {
      packets_received = twcc_received;
      packets_lost = packets_expected - twcc_received;
      if (packets_lost < 0) {
        packets_lost = 0;
      }
      loss_packets += packets_lost;
    }

    int64_t deliver_ms = -1;
    if (last_recv_ms >= 0 && last_recv_to_decode_ms >= 0) {
      deliver_ms = last_recv_ms + last_recv_to_decode_ms;
      deliver_to_decoder_ms[decode_info.rtp_timestamp] = deliver_ms;
    }

    int64_t decode_end = -1;
    if (deliver_ms >= 0 && decode_time_ms >= 0) {
      decode_end = deliver_ms + decode_time_ms;
      decode_end_ms[decode_info.rtp_timestamp] = decode_end;
    }

    if (enc_end_ms >= 0 && decode_end >= 0) {
      const int64_t e2e = decode_end - enc_end_ms;
      if (max_frame_e2e_ms < 0 || e2e > max_frame_e2e_ms) {
        max_frame_e2e_ms = e2e;
      }
    }

    if (enc_end_ms >= 0 && first_send_ms >= 0) {
      send_gaps.push_back(first_send_ms - enc_end_ms);
    }
    if (first_send_ms >= 0 && last_recv_ms >= 0) {
      net_gaps.push_back(last_recv_ms - first_send_ms);
    }
    if (deliver_ms >= 0 && last_recv_ms >= 0) {
      recv_gaps.push_back(deliver_ms - last_recv_ms);
    }
    if (decode_end >= 0 && deliver_ms >= 0) {
      dec_gaps.push_back(decode_end - deliver_ms);
    }

    int packets_recovered_rtx = 0;
    std::set<uint16_t> recovered_rtx_seqs;
    for (const auto& rtx_packet : frame_rtx_packets) {
      if (!rtx_packet.rtx_target_seq.has_value()) {
        continue;
      }
      const auto packet_time =
          twcc_correlator
              ? twcc_correlator->GetPacketTime(
                    rtx_packet.transport_sequence_number)
              : absl::nullopt;
      if (!packet_time.has_value()) {
        continue;
      }
      const int64_t recv_ms = packet_time->receive_time_us / 1000;
      if (deliver_ms >= 0 && recv_ms > deliver_ms) {
        continue;
      }
      recovered_rtx_seqs.insert(*rtx_packet.rtx_target_seq);
    }
    packets_recovered_rtx = static_cast<int>(recovered_rtx_seqs.size());

    int packets_recovered_fec = 0;
    std::set<uint16_t> recovered_fec_seqs;
    for (const auto& fec_info : fec_packet_window_) {
      const auto packet_time =
          twcc_correlator
              ? twcc_correlator->GetPacketTime(
                    fec_info.packet.transport_sequence_number)
              : absl::nullopt;
      if (!packet_time.has_value()) {
        continue;
      }
      const int64_t recv_ms = packet_time->receive_time_us / 1000;
      if (deliver_ms >= 0 && recv_ms > deliver_ms) {
        continue;
      }
      for (uint16_t seq : fec_info.protected_sequence_numbers) {
        auto it = seq_to_frame.find(seq);
        if (it != seq_to_frame.end() && it->second == decode_info.rtp_timestamp) {
          recovered_fec_seqs.insert(seq);
        }
      }
    }
    packets_recovered_fec = static_cast<int>(recovered_fec_seqs.size());

    FrameDigestEntry frame_entry;
    frame_entry.rtp_timestamp = decode_info.rtp_timestamp;
    frame_entry.is_keyframe = is_keyframe;
    frame_entry.frame_size_bytes = frame_size_bytes;
    frame_entry.packets_expected = packets_expected;
    frame_entry.capture_time_ms = capture_time_ms;
    frame_entry.enc_start_ms = enc_start_ms;
    frame_entry.enc_end_ms = enc_end_ms;
    frame_entry.first_send_ms = first_send_ms;
    frame_entry.last_send_ms = last_send_ms;
    frame_entry.first_recv_ms = first_recv_ms;
    frame_entry.last_recv_ms = last_recv_ms;
    frame_entry.deliver_to_decoder_ms = deliver_ms;
    frame_entry.decode_end_ms = decode_end;
    frame_entry.decode_time_ms = decode_time_ms;
    if (decode_info.decode_end_time_us > 0) {
      frame_entry.decode_end_time_ms_rx = decode_info.decode_end_time_us / 1000;
    }
    frame_entry.packets_received = packets_received;
    frame_entry.packets_lost = packets_lost;
    frame_entry.packets_recovered_rtx = packets_recovered_rtx;
    frame_entry.packets_recovered_fec = packets_recovered_fec;
    frame_digests.push_back(frame_entry);

    for (const auto& packet_info : media_packets) {
      PacketDigestEntry entry;
      entry.rtp_timestamp = decode_info.rtp_timestamp;
      entry.rtp_sequence_number = packet_info.rtp_sequence_number;
      entry.transport_sequence_number = packet_info.transport_sequence_number;
      entry.send_time_ms = packet_info.send_time_ms;
      entry.size_bytes = packet_info.size_bytes;
      entry.kind = packet_info.kind;
      if (twcc_correlator) {
        auto packet_time =
            twcc_correlator->GetPacketTime(packet_info.transport_sequence_number);
        if (packet_time.has_value()) {
          entry.recv_time_ms = packet_time->receive_time_us / 1000;
        }
      }
      packet_digests.push_back(entry);
    }

    for (const auto& packet_info : frame_rtx_packets) {
      PacketDigestEntry entry;
      entry.rtp_timestamp = decode_info.rtp_timestamp;
      entry.rtp_sequence_number = packet_info.rtp_sequence_number;
      entry.transport_sequence_number = packet_info.transport_sequence_number;
      entry.send_time_ms = packet_info.send_time_ms;
      entry.size_bytes = packet_info.size_bytes;
      entry.kind = packet_info.kind;
      entry.rtx_target_seq = packet_info.rtx_target_seq;
      if (twcc_correlator) {
        auto packet_time =
            twcc_correlator->GetPacketTime(packet_info.transport_sequence_number);
        if (packet_time.has_value()) {
          entry.recv_time_ms = packet_time->receive_time_us / 1000;
        }
      }
      packet_digests.push_back(entry);
    }
  }

  for (const auto& fec_info : fec_packet_window_) {
    std::set<uint32_t> target_frames;
    for (uint16_t seq : fec_info.protected_sequence_numbers) {
      auto it = seq_to_frame.find(seq);
      if (it == seq_to_frame.end()) {
        continue;
      }
      if (window_frames.count(it->second) > 0) {
        target_frames.insert(it->second);
      }
    }
    uint32_t target_frame_ts = 0;
    if (target_frames.size() == 1) {
      target_frame_ts = *target_frames.begin();
    }

    PacketDigestEntry entry;
    entry.rtp_timestamp = target_frame_ts;
    entry.rtp_sequence_number = fec_info.packet.rtp_sequence_number;
    entry.transport_sequence_number = fec_info.packet.transport_sequence_number;
    entry.send_time_ms = fec_info.packet.send_time_ms;
    entry.size_bytes = fec_info.packet.size_bytes;
    entry.kind = PacketKind::kFec;
    entry.fec_repair_id = fec_info.repair_id;
    if (twcc_correlator) {
      auto packet_time = twcc_correlator->GetPacketTime(
          fec_info.packet.transport_sequence_number);
      if (packet_time.has_value()) {
        entry.recv_time_ms = packet_time->receive_time_us / 1000;
      }
    }
    packet_digests.push_back(entry);

    if (entry.recv_time_ms >= 0) {
      ++fec_recv;
    }

    RepairDigestEntry repair_entry;
    repair_entry.repair_id = fec_info.repair_id;
    repair_entry.type = "FEC";
    constexpr size_t kMaxTargetSeqs = 10;
    if (target_frame_ts != 0) {
      repair_entry.target_frame_ts = target_frame_ts;
    }
    if (!fec_info.protected_sequence_numbers.empty()) {
      const size_t count = fec_info.protected_sequence_numbers.size();
      repair_entry.target_seq_count = static_cast<int>(count);
      const size_t limit = std::min(count, kMaxTargetSeqs);
      repair_entry.target_seqs.assign(fec_info.protected_sequence_numbers.begin(),
                                      fec_info.protected_sequence_numbers.begin() + limit);
    }
    repair_entry.repair_packet_count = 1;
    repair_entry.nack_request_ms = entry.send_time_ms;
    repair_entry.repair_first_send_ms = entry.send_time_ms;
    repair_entry.repair_last_send_ms = entry.send_time_ms;
    repair_entry.repair_packet_seqs.push_back(
        fec_info.packet.rtp_sequence_number);
    repair_entry.repair_first_recv_ms = entry.recv_time_ms;
    repair_entry.repair_last_recv_ms = entry.recv_time_ms;
    int64_t deliver_ms = -1;
    if (target_frame_ts != 0) {
      auto it = deliver_to_decoder_ms.find(target_frame_ts);
      if (it != deliver_to_decoder_ms.end()) {
        deliver_ms = it->second;
      }
    }
    constexpr size_t kMaxSeqsPerOutcome = 10;
    for (size_t i = 0; i < fec_info.protected_sequence_numbers.size(); ++i) {
      uint16_t seq = fec_info.protected_sequence_numbers[i];
      if (entry.recv_time_ms < 0) {
        ++repair_entry.failed_seq_count;
        if (repair_entry.failed_seqs.size() < kMaxSeqsPerOutcome) {
          repair_entry.failed_seqs.push_back(seq);
        }
      } else if (deliver_ms >= 0 && entry.recv_time_ms > deliver_ms) {
        ++repair_entry.too_late_seq_count;
        if (repair_entry.too_late_seqs.size() < kMaxSeqsPerOutcome) {
          repair_entry.too_late_seqs.push_back(seq);
        }
      } else {
        ++repair_entry.recovered_seq_count;
        if (repair_entry.recovered_seqs.size() < kMaxSeqsPerOutcome) {
          repair_entry.recovered_seqs.push_back(seq);
        }
      }
    }
    if (repair_entry.recovered_seq_count > 0 &&
        repair_entry.too_late_seq_count == 0 &&
        repair_entry.failed_seq_count == 0) {
      repair_entry.outcome = "RECOVERED";
    } else if (repair_entry.recovered_seq_count == 0 &&
               repair_entry.too_late_seq_count > 0 &&
               repair_entry.failed_seq_count == 0) {
      repair_entry.outcome = "TOO_LATE";
    } else if (repair_entry.recovered_seq_count == 0 &&
               repair_entry.too_late_seq_count == 0 &&
               repair_entry.failed_seq_count > 0) {
      repair_entry.outcome = "FAILED";
    } else {
      repair_entry.outcome = "MIXED";
    }
    if (repair_entry.repair_last_recv_ms >= 0 &&
        repair_entry.repair_first_send_ms >= 0) {
      repair_entry.duration_ms =
          repair_entry.repair_last_recv_ms - repair_entry.repair_first_send_ms;
    }
    repair_digests.push_back(std::move(repair_entry));
  }

  if (!rtx_packets_all.empty()) {
    std::vector<PacketTimingInfo> rtx_sorted = rtx_packets_all;
    std::sort(rtx_sorted.begin(), rtx_sorted.end(),
              [](const PacketTimingInfo& a, const PacketTimingInfo& b) {
                return a.send_time_ms < b.send_time_ms;
              });
    constexpr int64_t kNackBatchWindowMs = 5;
    std::vector<PacketTimingInfo> batch;
    int64_t batch_last_send_ms = -1;
    auto flush_batch = [&]() {
      if (batch.empty()) {
        return;
      }
      RepairDigestEntry repair_entry;
      repair_entry.type = "RTX_BATCH";
      repair_entry.repair_id = AllocateRepairId();
      repair_entry.repair_packet_count = static_cast<int>(batch.size());
      repair_entry.repair_first_send_ms = batch.front().send_time_ms;
      repair_entry.repair_last_send_ms = batch.back().send_time_ms;
      repair_entry.nack_request_ms = repair_entry.repair_first_send_ms;

      std::set<uint16_t> target_seqs_set;
      std::set<uint32_t> target_frames_set;
      std::map<uint32_t, int> target_frame_counts;
      for (const auto& packet : batch) {
        if (packet.rtx_target_seq.has_value()) {
          target_seqs_set.insert(*packet.rtx_target_seq);
          auto frame_it = seq_to_frame.find(*packet.rtx_target_seq);
          if (frame_it != seq_to_frame.end()) {
            target_frames_set.insert(frame_it->second);
            ++target_frame_counts[frame_it->second];
          }
        }
      }
      constexpr size_t kMaxTargetSeqs = 10;
      constexpr size_t kMaxSeqsPerOutcome = 10;
      repair_entry.target_seq_count =
          static_cast<int>(target_seqs_set.size());
      if (!target_seqs_set.empty()) {
        const size_t limit = std::min(target_seqs_set.size(), kMaxTargetSeqs);
        auto it = target_seqs_set.begin();
        for (size_t i = 0; i < limit; ++i, ++it) {
          repair_entry.target_seqs.push_back(*it);
        }
      }
      if (target_frames_set.size() == 1) {
        repair_entry.target_frame_ts = *target_frames_set.begin();
      }

      for (size_t i = 0; i < batch.size() && i < 3; ++i) {
        repair_entry.repair_packet_seqs.push_back(
            batch[i].rtp_sequence_number);
      }

      int64_t first_recv_ms = -1;
      int64_t last_recv_ms = -1;
      for (const auto& packet : batch) {
        if (!twcc_correlator) {
          continue;
        }
        auto packet_time =
            twcc_correlator->GetPacketTime(packet.transport_sequence_number);
        if (!packet_time.has_value()) {
          continue;
        }
        const int64_t recv_ms = packet_time->receive_time_us / 1000;
        if (first_recv_ms == -1 || recv_ms < first_recv_ms) {
          first_recv_ms = recv_ms;
        }
        if (last_recv_ms == -1 || recv_ms > last_recv_ms) {
          last_recv_ms = recv_ms;
        }
      }
      repair_entry.repair_first_recv_ms = first_recv_ms;
      repair_entry.repair_last_recv_ms = last_recv_ms;

      int64_t deliver_ms = -1;
      if (repair_entry.target_frame_ts.has_value()) {
        auto it = deliver_to_decoder_ms.find(*repair_entry.target_frame_ts);
        if (it != deliver_to_decoder_ms.end()) {
          deliver_ms = it->second;
        }
      } else {
        for (uint32_t frame_ts : target_frames_set) {
          auto it = deliver_to_decoder_ms.find(frame_ts);
          if (it == deliver_to_decoder_ms.end()) {
            continue;
          }
          if (deliver_ms == -1 || it->second < deliver_ms) {
            deliver_ms = it->second;
          }
        }
      }

      if (last_recv_ms < 0) {
        repair_entry.outcome = "FAILED";
      } else if (deliver_ms >= 0 && last_recv_ms > deliver_ms) {
        repair_entry.outcome = "TOO_LATE";
      } else {
        repair_entry.outcome = "RECOVERED";
      }
      struct OutcomeCounts {
        int recovered = 0;
        int too_late = 0;
        int failed = 0;
      };
      std::map<uint32_t, OutcomeCounts> frame_outcomes;
      for (uint16_t target_seq : target_seqs_set) {
        int64_t target_last_recv_ms = -1;
        for (const auto& packet : batch) {
          if (!packet.rtx_target_seq.has_value() ||
              *packet.rtx_target_seq != target_seq) {
            continue;
          }
          if (!twcc_correlator) {
            continue;
          }
          auto packet_time =
              twcc_correlator->GetPacketTime(packet.transport_sequence_number);
          if (!packet_time.has_value()) {
            continue;
          }
          const int64_t recv_ms = packet_time->receive_time_us / 1000;
          if (target_last_recv_ms == -1 || recv_ms > target_last_recv_ms) {
            target_last_recv_ms = recv_ms;
          }
        }
        int64_t target_deliver_ms = -1;
        auto frame_it = seq_to_frame.find(target_seq);
        if (frame_it != seq_to_frame.end()) {
          auto it = deliver_to_decoder_ms.find(frame_it->second);
          if (it != deliver_to_decoder_ms.end()) {
            target_deliver_ms = it->second;
          }
        }
        uint32_t frame_ts = 0;
        if (frame_it != seq_to_frame.end()) {
          frame_ts = frame_it->second;
        }
        if (target_last_recv_ms < 0) {
          ++repair_entry.failed_seq_count;
          if (repair_entry.failed_seqs.size() < kMaxSeqsPerOutcome) {
            repair_entry.failed_seqs.push_back(target_seq);
          }
          if (frame_ts != 0) {
            ++frame_outcomes[frame_ts].failed;
          }
        } else if (target_deliver_ms >= 0 &&
                   target_last_recv_ms > target_deliver_ms) {
          ++repair_entry.too_late_seq_count;
          if (repair_entry.too_late_seqs.size() < kMaxSeqsPerOutcome) {
            repair_entry.too_late_seqs.push_back(target_seq);
          }
          if (frame_ts != 0) {
            ++frame_outcomes[frame_ts].too_late;
          }
        } else {
          ++repair_entry.recovered_seq_count;
          if (repair_entry.recovered_seqs.size() < kMaxSeqsPerOutcome) {
            repair_entry.recovered_seqs.push_back(target_seq);
          }
          if (frame_ts != 0) {
            ++frame_outcomes[frame_ts].recovered;
          }
        }
      }
      if (!target_frame_counts.empty()) {
        std::ostringstream frames_summary;
        bool first = true;
        for (const auto& entry : target_frame_counts) {
          if (!first) {
            frames_summary << ",";
          }
          first = false;
          frames_summary << entry.first << "(" << entry.second << ")";
        }
        repair_entry.target_frames_summary = frames_summary.str();
      }
      if (!frame_outcomes.empty()) {
        std::ostringstream outcomes_summary;
        bool first = true;
        for (const auto& entry : frame_outcomes) {
          if (!first) {
            outcomes_summary << ";";
          }
          first = false;
          outcomes_summary << entry.first << ":r" << entry.second.recovered
                           << "/l" << entry.second.too_late << "/f"
                           << entry.second.failed;
        }
        repair_entry.frame_outcome_summary = outcomes_summary.str();
      }
      if (repair_entry.recovered_seq_count > 0 &&
          repair_entry.too_late_seq_count == 0 &&
          repair_entry.failed_seq_count == 0) {
        repair_entry.outcome = "RECOVERED";
      } else if (repair_entry.recovered_seq_count == 0 &&
                 repair_entry.too_late_seq_count > 0 &&
                 repair_entry.failed_seq_count == 0) {
        repair_entry.outcome = "TOO_LATE";
      } else if (repair_entry.recovered_seq_count == 0 &&
                 repair_entry.too_late_seq_count == 0 &&
                 repair_entry.failed_seq_count > 0) {
        repair_entry.outcome = "FAILED";
      } else {
        repair_entry.outcome = "MIXED";
      }
      if (repair_entry.repair_last_recv_ms >= 0 &&
          repair_entry.repair_first_send_ms >= 0) {
        repair_entry.duration_ms =
            repair_entry.repair_last_recv_ms - repair_entry.repair_first_send_ms;
      }
      repair_digests.push_back(std::move(repair_entry));
      batch.clear();
      batch_last_send_ms = -1;
    };

    for (const auto& packet : rtx_sorted) {
      if (batch.empty()) {
        batch.push_back(packet);
        batch_last_send_ms = packet.send_time_ms;
        continue;
      }
      if (packet.send_time_ms - batch_last_send_ms <= kNackBatchWindowMs) {
        batch.push_back(packet);
        batch_last_send_ms = packet.send_time_ms;
        continue;
      }
      flush_batch();
      batch.push_back(packet);
      batch_last_send_ms = packet.send_time_ms;
    }
    flush_batch();
  }

  for (const auto& digest : packet_digests) {
    if (digest.recv_time_ms >= 0 && digest.kind == PacketKind::kRtx) {
      ++rtx_recv;
    }
    if (digest.recv_time_ms >= 0) {
      const int64_t one_way = digest.recv_time_ms - digest.send_time_ms;
      one_way_delays.push_back(one_way);
    }
  }

  std::vector<ControlDigestEntry> control_digests(control_digests_.begin(),
                                                  control_digests_.end());
  int64_t min_time_ms = -1;
  int64_t min_capture_ms = -1;
  int64_t max_capture_ms = -1;
  int64_t max_last_recv_ms = -1;
  for (const auto& frame : frame_digests) {
    UpdateMinTimeMs(frame.capture_time_ms, &min_time_ms);
    UpdateMinTimeMs(frame.enc_start_ms, &min_time_ms);
    UpdateMinTimeMs(frame.enc_end_ms, &min_time_ms);
    UpdateMinTimeMs(frame.first_send_ms, &min_time_ms);
    UpdateMinTimeMs(frame.last_send_ms, &min_time_ms);
    UpdateMinTimeMs(frame.first_recv_ms, &min_time_ms);
    UpdateMinTimeMs(frame.last_recv_ms, &min_time_ms);
    UpdateMinTimeMs(frame.deliver_to_decoder_ms, &min_time_ms);
    UpdateMinTimeMs(frame.decode_end_ms, &min_time_ms);
    UpdateMinTimeMs(frame.capture_time_ms, &min_capture_ms);
    UpdateMaxTimeMs(frame.capture_time_ms, &max_capture_ms);
    UpdateMaxTimeMs(frame.last_recv_ms, &max_last_recv_ms);
  }
  for (const auto& packet : packet_digests) {
    UpdateMinTimeMs(packet.send_time_ms, &min_time_ms);
    UpdateMinTimeMs(packet.recv_time_ms, &min_time_ms);
  }
  for (const auto& repair : repair_digests) {
    UpdateMinTimeMs(repair.nack_request_ms, &min_time_ms);
    UpdateMinTimeMs(repair.repair_first_send_ms, &min_time_ms);
    UpdateMinTimeMs(repair.repair_last_send_ms, &min_time_ms);
    UpdateMinTimeMs(repair.repair_first_recv_ms, &min_time_ms);
    UpdateMinTimeMs(repair.repair_last_recv_ms, &min_time_ms);
  }
  for (const auto& control : control_digests) {
    UpdateMinTimeMs(control.time_ms, &min_time_ms);
  }
  const int64_t time_base_ms = (min_time_ms >= 0) ? min_time_ms : 0;
  const int64_t max_window_ms =
      (max_last_recv_ms >= 0) ? max_last_recv_ms : max_capture_ms;
  const bool has_capture_window =
      (min_capture_ms >= 0 && max_window_ms >= 0);
  auto control_in_window = [&](int64_t time_ms) {
    if (!has_capture_window) {
      return true;
    }
    return time_ms >= min_capture_ms && time_ms <= max_window_ms;
  };

  std::unordered_set<uint32_t> important_rtp_timestamps;
  important_rtp_timestamps.insert(stall_rtp_timestamp);
  for (const auto& frame : frame_digests) {
    if (frame.packets_lost > 0 || frame.packets_recovered_rtx > 0 ||
        frame.packets_recovered_fec > 0) {
      important_rtp_timestamps.insert(frame.rtp_timestamp);
    }
  }
  auto is_important_packet = [&](const PacketDigestEntry& packet) {
    if (packet.kind != PacketKind::kMedia) {
      return true;
    }
    if (packet.recv_time_ms < 0) {
      return true;
    }
    return important_rtp_timestamps.count(packet.rtp_timestamp) > 0;
  };

  const int64_t p95_send_gap = Percentile(send_gaps, 0.95);
  const int64_t p95_net_gap = Percentile(net_gaps, 0.95);
  const int64_t p95_recv_gap = Percentile(recv_gaps, 0.95);
  const int64_t p95_dec_gap = Percentile(dec_gaps, 0.95);
  const int64_t p50_one_way = Percentile(one_way_delays, 0.50);
  const int64_t p95_one_way = Percentile(one_way_delays, 0.95);

  std::unordered_map<uint32_t, std::vector<const PacketDigestEntry*>>
      abnormal_packets_by_frame;
  for (const auto& packet : packet_digests) {
    if (!is_important_packet(packet)) {
      continue;
    }
    const int64_t one_way_ms =
        (packet.send_time_ms >= 0 && packet.recv_time_ms >= 0)
            ? (packet.recv_time_ms - packet.send_time_ms)
            : -1;
    const bool is_repair = packet.kind != PacketKind::kMedia;
    const bool is_lost = packet.recv_time_ms < 0;
    const bool is_delayed = one_way_ms >= 0 && p95_one_way >= 0 &&
                            one_way_ms > (p95_one_way + 20);
    if (is_repair || is_lost || is_delayed) {
      abnormal_packets_by_frame[packet.rtp_timestamp].push_back(&packet);
    }
  }
  std::vector<std::pair<int64_t, uint32_t>> encoder_points_abs;
  struct ControlLine {
    int64_t time_ms = -1;
    std::string text;
  };
  std::vector<ControlLine> control_lines;
  control_lines.reserve(control_digests.size() + repair_digests.size());
  for (const auto& c : control_digests) {
    if (!control_in_window(c.time_ms)) {
      continue;
    }
    if (c.type == ControlDigestEntry::Type::kRtcpFeedback) {
      control_lines.push_back({c.time_ms, "<fb> kind=" + c.feedback_kind});
    } else if (c.type == ControlDigestEntry::Type::kRtcpSignal) {
      std::set<std::string> kinds;
      kinds.insert("NACK");
      if (c.rtcp_counter.pli_packets > 0) {
        kinds.insert("PLI");
      }
      if (c.rtcp_counter.fir_packets > 0) {
        kinds.insert("FIR");
      }
      std::string joined;
      for (auto it = kinds.begin(); it != kinds.end(); ++it) {
        if (it != kinds.begin()) {
          joined.append(",");
        }
        joined.append(*it);
      }
      control_lines.push_back({c.time_ms, "<fb> kind=" + joined});
    } else if (c.type == ControlDigestEntry::Type::kBweTargetRate) {
      control_lines.push_back(
          {c.time_ms, "<rate> bwe_target=" + FormatBitrateLabel(c.bitrate_bps)});
    } else if (c.type == ControlDigestEntry::Type::kEncoderTargetRate) {
      control_lines.push_back({c.time_ms,
                               "<rate> encoder_target=" +
                                   FormatBitrateLabel(c.bitrate_bps)});
      encoder_points_abs.emplace_back(c.time_ms, c.bitrate_bps);
    }
  }
  for (const auto& repair : repair_digests) {
    if (std::string(repair.type) != "RTX_BATCH") {
      continue;
    }
    int64_t event_time_ms = repair.repair_first_send_ms;
    if (event_time_ms < 0) {
      event_time_ms = repair.nack_request_ms;
    }
    if (event_time_ms < 0) {
      event_time_ms = repair.repair_last_recv_ms;
    }
    std::ostringstream oss;
    oss << "<lr> repair_id=" << repair.repair_id
        << " target_frames="
        << (!repair.target_frames_summary.empty() ? repair.target_frames_summary
                                                  : "0")
        << " packet_count=" << repair.repair_packet_count
        << " outcome=" << repair.outcome
        << " frame_outcomes="
        << (!repair.frame_outcome_summary.empty() ? repair.frame_outcome_summary
                                                  : "-")
        << " duration_ms=" << repair.duration_ms
        << " last_recv_ms="
        << RelativeTimeMs(repair.repair_last_recv_ms, time_base_ms);
    control_lines.push_back({event_time_ms, oss.str()});
  }
  std::sort(control_lines.begin(), control_lines.end(),
            [](const ControlLine& a, const ControlLine& b) {
              const int64_t a_key = (a.time_ms >= 0)
                                        ? a.time_ms
                                        : std::numeric_limits<int64_t>::max();
              const int64_t b_key = (b.time_ms >= 0)
                                        ? b.time_ms
                                        : std::numeric_limits<int64_t>::max();
              if (a_key != b_key) {
                return a_key < b_key;
              }
              return a.text < b.text;
            });

  RTC_LOG(LS_INFO) << "[event_context]";
  RTC_LOG(LS_INFO) << "codec=" << report_header_.codec
                   << " resolutionn=" << report_header_.width << "x"
                   << report_header_.height
                   << " fps_nominal=" << report_header_.fps_nominal
                   << " rtx_enabled=" << (report_header_.rtx_enabled ? 1 : 0)
                   << " fec_enabled=" << (report_header_.fec_enabled ? 1 : 0)
                   << " time_base_ms=" << time_base_ms;
  RTC_LOG(LS_INFO) << "stall_rtp_ts=" << stall_rtp_timestamp
                   << " stall_gap_ms=" << stall_gap_ms
                   << " stall_report_size_bytes=" << stall_report_size_bytes
                   << " frame_count_in_window=" << decode_delays.size();
  RTC_LOG(LS_INFO) << "max_frame_e2e_ms=" << max_frame_e2e_ms
                   << " loss_packets=" << loss_packets
                   << " nack_sent=" << nack_sent
                   << " rtx_recv=" << rtx_recv
                   << " fec_recv=" << fec_recv
                   << " p50_one_way_ms=" << p50_one_way
                   << " p95_one_way_ms=" << p95_one_way;
  RTC_LOG(LS_INFO) << "p95_send_gap_ms=" << p95_send_gap
                   << " p95_net_gap_ms=" << p95_net_gap
                   << " p95_recv_gap_ms=" << p95_recv_gap
                   << " p95_dec_gap_ms=" << p95_dec_gap;
  RTC_LOG(LS_INFO) << "";
  RTC_LOG(LS_INFO) << "[TIMELINE_DIGEST]";
  int64_t prev_capture_time_ms = -1;
  int64_t prev_decode_end_time_ms_rx = -1;
  for (size_t i = 0; i < frame_digests.size(); ++i) {
    const auto& f = frame_digests[i];
    const auto gap_or_minus_one = [](int64_t start_ms, int64_t end_ms) {
      if (start_ms < 0 || end_ms < 0) {
        return int64_t{-1};
      }
      return end_ms - start_ms;
    };
    const int64_t capture_ms = RelativeTimeMs(f.capture_time_ms, time_base_ms);
    const int64_t prev_capture_gap_ms =
        gap_or_minus_one(prev_capture_time_ms, f.capture_time_ms);
    const int64_t capture_to_enc_ms =
        gap_or_minus_one(f.capture_time_ms, f.enc_start_ms);
    const int64_t encode_ms = gap_or_minus_one(f.enc_start_ms, f.enc_end_ms);
    const int64_t enc_to_first_send_ms =
        gap_or_minus_one(f.enc_end_ms, f.first_send_ms);
    const int64_t first_send_to_last_recv_ms =
        gap_or_minus_one(f.first_send_ms, f.last_recv_ms);
    const int64_t last_recv_to_deliver_ms =
        gap_or_minus_one(f.last_recv_ms, f.deliver_to_decoder_ms);
    const int64_t decode_ms = f.decode_time_ms;
    const int64_t prev_decode_end_gap_ms =
        gap_or_minus_one(prev_decode_end_time_ms_rx, f.decode_end_time_ms_rx);
    const int stall_flag = (f.rtp_timestamp == stall_rtp_timestamp) ? 1 : 0;
    RTC_LOG(LS_INFO) << capture_ms << " rtpts=" << f.rtp_timestamp
                     << " gaps=(" << prev_capture_gap_ms << " "
                     << capture_to_enc_ms << " " << encode_ms << " "
                     << enc_to_first_send_ms << " "
                     << first_send_to_last_recv_ms << " "
                     << last_recv_to_deliver_ms << " " << decode_ms << ")"
                     << " stall=" << (stall_flag ? "True" : "False")
                     << " prev_gap=" << prev_decode_end_gap_ms;
    auto packet_it = abnormal_packets_by_frame.find(f.rtp_timestamp);
    if (packet_it != abnormal_packets_by_frame.end() &&
        !packet_it->second.empty()) {
      auto frame_packets = packet_it->second;
      std::sort(frame_packets.begin(), frame_packets.end(),
                [](const PacketDigestEntry* a, const PacketDigestEntry* b) {
                  if (a->rtp_sequence_number != b->rtp_sequence_number) {
                    return a->rtp_sequence_number < b->rtp_sequence_number;
                  }
                  const int64_t a_send =
                      (a->send_time_ms >= 0)
                          ? a->send_time_ms
                          : std::numeric_limits<int64_t>::max();
                  const int64_t b_send =
                      (b->send_time_ms >= 0)
                          ? b->send_time_ms
                          : std::numeric_limits<int64_t>::max();
                  return a_send < b_send;
                });
      size_t idx = 0;
      while (idx < frame_packets.size()) {
        const PacketDigestEntry* packet = frame_packets[idx];
        if (packet->recv_time_ms < 0 && packet->kind == PacketKind::kMedia) {
          const uint16_t start_seq = packet->rtp_sequence_number;
          uint16_t end_seq = start_seq;
          size_t j = idx + 1;
          while (j < frame_packets.size()) {
            const PacketDigestEntry* next = frame_packets[j];
            if (next->kind != PacketKind::kMedia || next->recv_time_ms >= 0) {
              break;
            }
            if (static_cast<uint16_t>(end_seq + 1) !=
                next->rtp_sequence_number) {
              break;
            }
            end_seq = next->rtp_sequence_number;
            ++j;
          }
          RTC_LOG(LS_INFO) << "  pkt loss seq="
                           << (start_seq == end_seq
                                   ? std::to_string(start_seq)
                                   : std::to_string(start_seq) + "-" +
                                         std::to_string(end_seq))
                           << " count=" << (end_seq - start_seq + 1);
          idx = j;
          continue;
        }
        const int64_t one_way_ms =
            (packet->send_time_ms >= 0 && packet->recv_time_ms >= 0)
                ? (packet->recv_time_ms - packet->send_time_ms)
                : -1;
        std::string pkt_tag;
        if (packet->kind != PacketKind::kMedia) {
          pkt_tag.append("REPAIR");
        }
        if (one_way_ms >= 0 && p95_one_way >= 0 && one_way_ms > (p95_one_way + 20)) {
          if (!pkt_tag.empty()) {
            pkt_tag.append("|");
          }
          pkt_tag.append("DELAY");
        }
        RTC_LOG(LS_INFO) << "  pkt kind=" << PacketKindToString(packet->kind)
                         << " seq=" << packet->rtp_sequence_number
                         << " send_ms="
                         << RelativeTimeMs(packet->send_time_ms, time_base_ms)
                         << " recv_ms="
                         << RelativeTimeMs(packet->recv_time_ms, time_base_ms)
                         << " one_way_ms=" << one_way_ms
                         << " tag=" << (pkt_tag.empty() ? "-" : pkt_tag);
        ++idx;
      }
    }
    prev_capture_time_ms = f.capture_time_ms;
    prev_decode_end_time_ms_rx = f.decode_end_time_ms_rx;
  }
  RTC_LOG(LS_INFO) << "";
  RTC_LOG(LS_INFO) << "[control_digest]";
  if (control_lines.empty()) {
    RTC_LOG(LS_INFO) << "none";
  } else {
    for (const auto& line : control_lines) {
      RTC_LOG(LS_INFO) << RelativeTimeMs(line.time_ms, time_base_ms)
                       << " " << line.text;
    }
  }
  if (report_header_.fps_nominal > 0 && !encoder_points_abs.empty()) {
    std::sort(encoder_points_abs.begin(), encoder_points_abs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    struct SizeOutlier {
      uint32_t rtp_ts;
      size_t size_bytes;
      int64_t expected_bytes;
      double ratio;
    };
    std::vector<SizeOutlier> over;
    std::vector<SizeOutlier> under;
    for (const auto& frame : frame_digests) {
      if (frame.capture_time_ms < 0 || frame.frame_size_bytes == 0) {
        continue;
      }
      auto it = std::upper_bound(
          encoder_points_abs.begin(), encoder_points_abs.end(),
          frame.capture_time_ms,
          [](int64_t value, const auto& entry) { return value < entry.first; });
      if (it == encoder_points_abs.begin()) {
        continue;
      }
      --it;
      const uint32_t bps = it->second;
      if (bps == 0) {
        continue;
      }
      const double expected =
          static_cast<double>(bps) / 8.0 / report_header_.fps_nominal;
      if (expected <= 0.0) {
        continue;
      }
      const double ratio = frame.frame_size_bytes / expected;
      if (ratio > 1.3) {
        over.push_back(SizeOutlier{frame.rtp_timestamp, frame.frame_size_bytes,
                                   static_cast<int64_t>(std::round(expected)),
                                   ratio});
      } else if (ratio < 0.7) {
        under.push_back(SizeOutlier{frame.rtp_timestamp, frame.frame_size_bytes,
                                    static_cast<int64_t>(std::round(expected)),
                                    ratio});
      }
    }
    auto by_ratio_desc = [](const SizeOutlier& a, const SizeOutlier& b) {
      return a.ratio > b.ratio;
    };
    auto by_ratio_asc = [](const SizeOutlier& a, const SizeOutlier& b) {
      return a.ratio < b.ratio;
    };
    std::sort(over.begin(), over.end(), by_ratio_desc);
    std::sort(under.begin(), under.end(), by_ratio_asc);
    const size_t kMaxOutliers = 5;
    std::ostringstream over_line;
    std::ostringstream under_line;
    for (size_t i = 0; i < over.size() && i < kMaxOutliers; ++i) {
      if (i > 0) {
        over_line << ";";
      }
      over_line << "rtpts=" << over[i].rtp_ts
                << " size=" << over[i].size_bytes
                << " exp=" << over[i].expected_bytes
                << " ratio=" << std::fixed << std::setprecision(2)
                << over[i].ratio;
    }
    for (size_t i = 0; i < under.size() && i < kMaxOutliers; ++i) {
      if (i > 0) {
        under_line << ";";
      }
      under_line << "rtpts=" << under[i].rtp_ts
                 << " size=" << under[i].size_bytes
                 << " exp=" << under[i].expected_bytes
                 << " ratio=" << std::fixed << std::setprecision(2)
                 << under[i].ratio;
    }
    RTC_LOG(LS_INFO) << "ENCODER_FRAME_SIZE_OUTLIERS over="
                     << (over_line.str().empty() ? "none" : over_line.str())
                     << " under="
                     << (under_line.str().empty() ? "none" : under_line.str());
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
