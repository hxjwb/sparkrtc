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
#include <map>
#include <set>
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
    uint32_t nack_sent,
    TwccTimeCorrelator* twcc_correlator) {
  RTC_LOG(LS_INFO) << "=== Profiling Report ===";
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
    for (const auto& packet_info : media_packets) {
      if (first_send_ms == -1 || packet_info.send_time_ms < first_send_ms) {
        first_send_ms = packet_info.send_time_ms;
      }
      if (last_send_ms == -1 || packet_info.send_time_ms > last_send_ms) {
        last_send_ms = packet_info.send_time_ms;
      }
      if (twcc_correlator) {
        auto packet_time =
            twcc_correlator->GetPacketTime(packet_info.transport_sequence_number);
        if (packet_time.has_value()) {
          int64_t recv_time_ms = packet_time->receive_time_us / 1000;
          if (first_recv_ms == -1 || recv_time_ms < first_recv_ms) {
            first_recv_ms = recv_time_ms;
          }
          if (last_recv_ms == -1 || recv_time_ms > last_recv_ms) {
            last_recv_ms = recv_time_ms;
          }
          ++twcc_received;
        }
      }
    }

    const int packets_expected = static_cast<int>(media_packets.size());
    if (twcc_received > 0) {
      packets_received = twcc_received;
      if (twcc_received == packets_expected) {
        packets_lost = 0;
      } else {
        packets_lost = -1;
      }
      if (packets_lost >= 0) {
        loss_packets += packets_lost;
      }
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
    frame_entry.packets_received = packets_received;
    frame_entry.packets_lost = packets_lost;
    frame_entry.packets_recovered_rtx = packets_recovered_rtx;
    frame_entry.packets_recovered_fec = packets_recovered_fec;
    frame_digests.push_back(frame_entry);

    RTC_LOG(LS_INFO) << "PRFL_FRAME rtp_ts=" << decode_info.rtp_timestamp
                     << " frame_type=" << (is_keyframe ? "key" : "delta")
                     << " frame_size_bytes=" << frame_size_bytes
                     << " packets_expected=" << packets_expected
                     << " capture_ms=" << capture_time_ms
                     << " enc_start_ms=" << enc_start_ms
                     << " enc_end_ms=" << enc_end_ms
                     << " first_send_ms=" << first_send_ms
                     << " last_send_ms=" << last_send_ms
                     << " first_recv_ms=" << first_recv_ms
                     << " last_recv_ms=" << last_recv_ms
                     << " deliver_to_decoder_ms=" << deliver_ms
                     << " decode_end_ms=" << decode_end
                     << " last_recv_to_decode_ms=" << last_recv_to_decode_ms
                     << " decode_time_ms=" << decode_time_ms
                     << " packets_received=" << packets_received
                     << " packets_lost=" << packets_lost
                     << " packets_recovered_rtx=" << packets_recovered_rtx
                     << " packets_recovered_fec=" << packets_recovered_fec
                     << " nack_count_for_frame=-1";

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
      for (const auto& packet : batch) {
        if (packet.rtx_target_seq.has_value()) {
          target_seqs_set.insert(*packet.rtx_target_seq);
          auto frame_it = seq_to_frame.find(*packet.rtx_target_seq);
          if (frame_it != seq_to_frame.end()) {
            target_frames_set.insert(frame_it->second);
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
        if (target_last_recv_ms < 0) {
          ++repair_entry.failed_seq_count;
          if (repair_entry.failed_seqs.size() < kMaxSeqsPerOutcome) {
            repair_entry.failed_seqs.push_back(target_seq);
          }
        } else if (target_deliver_ms >= 0 &&
                   target_last_recv_ms > target_deliver_ms) {
          ++repair_entry.too_late_seq_count;
          if (repair_entry.too_late_seqs.size() < kMaxSeqsPerOutcome) {
            repair_entry.too_late_seqs.push_back(target_seq);
          }
        } else {
          ++repair_entry.recovered_seq_count;
          if (repair_entry.recovered_seqs.size() < kMaxSeqsPerOutcome) {
            repair_entry.recovered_seqs.push_back(target_seq);
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
    RTC_LOG(LS_INFO) << "PRFL_PKT rtp_ts=" << digest.rtp_timestamp
                     << " kind=" << PacketKindToString(digest.kind)
                     << " rtp_seq=" << digest.rtp_sequence_number
                     << " transport_seq=" << digest.transport_sequence_number
                     << " send_ms=" << digest.send_time_ms
                     << " recv_ms=" << digest.recv_time_ms
                     << " size_bytes=" << digest.size_bytes
                     << " rtx_target_seq="
                     << (digest.rtx_target_seq.has_value()
                             ? std::to_string(*digest.rtx_target_seq)
                             : "-1")
                     << " repair_id="
                     << (digest.fec_repair_id.has_value()
                             ? std::to_string(*digest.fec_repair_id)
                             : "-1");
  }

  const int64_t p95_send_gap = Percentile(send_gaps, 0.95);
  const int64_t p95_net_gap = Percentile(net_gaps, 0.95);
  const int64_t p95_recv_gap = Percentile(recv_gaps, 0.95);
  const int64_t p95_dec_gap = Percentile(dec_gaps, 0.95);
  const int64_t p50_one_way = Percentile(one_way_delays, 0.50);
  const int64_t p95_one_way = Percentile(one_way_delays, 0.95);

  std::vector<std::string> tags;
  if (loss_packets >= 3 || nack_sent >= 3 || rtx_recv > 0 || fec_recv > 0) {
    tags.push_back("LOSSY");
  }
  if (p95_one_way >= 0 && p50_one_way >= 0 && (p95_one_way - p50_one_way) > 60) {
    tags.push_back("NET_DELAY_JUMP");
  }
  const int64_t recv_plus_dec =
      (p95_recv_gap >= 0 && p95_dec_gap >= 0) ? (p95_recv_gap + p95_dec_gap)
                                              : -1;
  const int64_t send_dom_threshold =
      std::max<int64_t>(60, std::max(p95_net_gap, recv_plus_dec));
  if (p95_send_gap > send_dom_threshold) {
    tags.push_back("SEND_GAP_DOMINANT");
  }
  const int64_t recv_dom_threshold =
      std::max<int64_t>(60, std::max(p95_send_gap, std::max(p95_net_gap, p95_dec_gap)));
  if (p95_recv_gap > recv_dom_threshold) {
    tags.push_back("RECV_GAP_DOMINANT");
  }
  if (p95_dec_gap > 40) {
    tags.push_back("DECODE_SLOW");
  }
  if (tags.size() > 2) {
    tags.resize(2);
  }

  std::string tags_joined;
  for (size_t i = 0; i < tags.size(); ++i) {
    if (i > 0) {
      tags_joined.append(",");
    }
    tags_joined.append(tags[i]);
  }

  RTC_LOG(LS_INFO) << "PRFL_SUM frame_count_in_window=" << decode_delays.size()
                   << " stall_gap_ms=" << stall_gap_ms
                   << " max_frame_e2e_ms=" << max_frame_e2e_ms
                   << " loss_packets=" << loss_packets
                   << " nack_sent=" << nack_sent
                   << " rtx_recv=" << rtx_recv
                   << " fec_recv=" << fec_recv
                   << " p95_send_gap_ms=" << p95_send_gap
                   << " p95_net_gap_ms=" << p95_net_gap
                   << " p95_recv_gap_ms=" << p95_recv_gap
                   << " p95_dec_gap_ms=" << p95_dec_gap
                   << " p50_one_way_ms=" << p50_one_way
                   << " p95_one_way_ms=" << p95_one_way
                   << " coarse_tags=" << tags_joined;

  for (const auto& repair : repair_digests) {
    std::string repair_seqs;
    for (size_t i = 0; i < repair.repair_packet_seqs.size(); ++i) {
      if (i > 0) {
        repair_seqs.append(",");
      }
      repair_seqs.append(std::to_string(repair.repair_packet_seqs[i]));
    }
    std::string target_seqs;
    for (size_t i = 0; i < repair.target_seqs.size(); ++i) {
      if (i > 0) {
        target_seqs.append(",");
      }
      target_seqs.append(std::to_string(repair.target_seqs[i]));
    }
    std::string recovered_seqs;
    for (size_t i = 0; i < repair.recovered_seqs.size(); ++i) {
      if (i > 0) {
        recovered_seqs.append(",");
      }
      recovered_seqs.append(std::to_string(repair.recovered_seqs[i]));
    }
    std::string too_late_seqs;
    for (size_t i = 0; i < repair.too_late_seqs.size(); ++i) {
      if (i > 0) {
        too_late_seqs.append(",");
      }
      too_late_seqs.append(std::to_string(repair.too_late_seqs[i]));
    }
    std::string failed_seqs;
    for (size_t i = 0; i < repair.failed_seqs.size(); ++i) {
      if (i > 0) {
        failed_seqs.append(",");
      }
      failed_seqs.append(std::to_string(repair.failed_seqs[i]));
    }
    RTC_LOG(LS_INFO) << "PRFL_REPAIR repair_id=" << repair.repair_id
                     << " type=" << repair.type
                     << " target_frame_ts="
                     << (repair.target_frame_ts.has_value()
                             ? std::to_string(*repair.target_frame_ts)
                             : "0")
                     << " target_seqs="
                     << (!target_seqs.empty() ? target_seqs : "-")
                     << " target_seq_count=" << repair.target_seq_count
                     << " repair_packet_seqs=" << repair_seqs
                     << " repair_packet_count=" << repair.repair_packet_count
                     << " nack_request_ms=" << repair.nack_request_ms
                     << " repair_first_send_ms=" << repair.repair_first_send_ms
                     << " repair_last_send_ms=" << repair.repair_last_send_ms
                     << " repair_first_recv_ms=" << repair.repair_first_recv_ms
                     << " repair_last_recv_ms=" << repair.repair_last_recv_ms
                     << " duration_ms=" << repair.duration_ms
                     << " recovered_seqs="
                     << (!recovered_seqs.empty() ? recovered_seqs : "-")
                     << " recovered_seq_count=" << repair.recovered_seq_count
                     << " too_late_seqs="
                     << (!too_late_seqs.empty() ? too_late_seqs : "-")
                     << " too_late_seq_count=" << repair.too_late_seq_count
                     << " failed_seqs="
                     << (!failed_seqs.empty() ? failed_seqs : "-")
                     << " failed_seq_count=" << repair.failed_seq_count
                     << " outcome=" << repair.outcome
                     ;
  }

  RTC_LOG(LS_INFO) << "[STALL_HEADER]";
  RTC_LOG(LS_INFO) << "codec=" << report_header_.codec
                   << " resolution=" << report_header_.width << "x"
                   << report_header_.height
                   << " fps_nominal=" << report_header_.fps_nominal
                   << " rtx_enabled=" << (report_header_.rtx_enabled ? 1 : 0)
                   << " fec_enabled=" << (report_header_.fec_enabled ? 1 : 0);
  RTC_LOG(LS_INFO) << "stall_rtp_ts=" << stall_rtp_timestamp
                   << " stall_gap_ms=" << stall_gap_ms
                   << " frame_count_in_window=" << decode_delays.size();
  RTC_LOG(LS_INFO) << "";
  RTC_LOG(LS_INFO) << "[WINDOW_SUMMARY]";
  RTC_LOG(LS_INFO) << "max_frame_e2e_ms=" << max_frame_e2e_ms
                   << " loss_packets=" << loss_packets
                   << " nack_sent=" << nack_sent
                   << " rtx_recv=" << rtx_recv
                   << " fec_recv=" << fec_recv;
  RTC_LOG(LS_INFO) << "p50_one_way_ms=" << p50_one_way
                   << " p95_one_way_ms=" << p95_one_way;
  RTC_LOG(LS_INFO) << "p95_send_gap_ms=" << p95_send_gap
                   << " p95_net_gap_ms=" << p95_net_gap
                   << " p95_recv_gap_ms=" << p95_recv_gap
                   << " p95_dec_gap_ms=" << p95_dec_gap;
  RTC_LOG(LS_INFO) << "coarse_tags=" << tags_joined;
  RTC_LOG(LS_INFO) << "";
  RTC_LOG(LS_INFO) << "[FRAME_DIGEST]";
  for (size_t i = 0; i < frame_digests.size(); ++i) {
    const auto& f = frame_digests[i];
    RTC_LOG(LS_INFO) << (i + 1) << ") rtp_ts=" << f.rtp_timestamp
                     << " frame_type=" << (f.is_keyframe ? "key" : "delta")
                     << " frame_size_bytes=" << f.frame_size_bytes
                     << " packets_expected=" << f.packets_expected
                     << " capture_ms=" << f.capture_time_ms
                     << " enc_start_ms=" << f.enc_start_ms
                     << " enc_end_ms=" << f.enc_end_ms
                     << " first_send_ms=" << f.first_send_ms
                     << " last_send_ms=" << f.last_send_ms
                     << " first_recv_ms=" << f.first_recv_ms
                     << " last_recv_ms=" << f.last_recv_ms
                     << " deliver_to_decoder_ms=" << f.deliver_to_decoder_ms
                     << " decode_end_ms=" << f.decode_end_ms
                     << " decode_time_ms=" << f.decode_time_ms
                     << " packets_received=" << f.packets_received
                     << " packets_lost=" << f.packets_lost
                     << " packets_recovered_rtx=" << f.packets_recovered_rtx
                     << " packets_recovered_fec=" << f.packets_recovered_fec;
  }
  RTC_LOG(LS_INFO) << "";
  RTC_LOG(LS_INFO) << "[PACKET_DIGEST]";
  for (size_t i = 0; i < packet_digests.size(); ++i) {
    const auto& p = packet_digests[i];
    RTC_LOG(LS_INFO) << (i + 1) << ") seq=" << p.rtp_sequence_number
                     << " rtp_ts=" << p.rtp_timestamp
                     << " send_ms=" << p.send_time_ms
                     << " recv_ms=" << p.recv_time_ms
                     << " size_bytes=" << p.size_bytes
                     << " kind=" << PacketKindToString(p.kind);
  }
  RTC_LOG(LS_INFO) << "";
  RTC_LOG(LS_INFO) << "[REPAIR_DIGEST]";
  for (size_t i = 0; i < repair_digests.size(); ++i) {
    const auto& r = repair_digests[i];
    std::string repair_seqs;
    for (size_t j = 0; j < r.repair_packet_seqs.size(); ++j) {
      if (j > 0) {
        repair_seqs.append(",");
      }
      repair_seqs.append(std::to_string(r.repair_packet_seqs[j]));
    }
    std::string target_seqs;
    for (size_t j = 0; j < r.target_seqs.size(); ++j) {
      if (j > 0) {
        target_seqs.append(",");
      }
      target_seqs.append(std::to_string(r.target_seqs[j]));
    }
    std::string recovered_seqs;
    for (size_t j = 0; j < r.recovered_seqs.size(); ++j) {
      if (j > 0) {
        recovered_seqs.append(",");
      }
      recovered_seqs.append(std::to_string(r.recovered_seqs[j]));
    }
    std::string too_late_seqs;
    for (size_t j = 0; j < r.too_late_seqs.size(); ++j) {
      if (j > 0) {
        too_late_seqs.append(",");
      }
      too_late_seqs.append(std::to_string(r.too_late_seqs[j]));
    }
    std::string failed_seqs;
    for (size_t j = 0; j < r.failed_seqs.size(); ++j) {
      if (j > 0) {
        failed_seqs.append(",");
      }
      failed_seqs.append(std::to_string(r.failed_seqs[j]));
    }
    RTC_LOG(LS_INFO) << (i + 1) << ") repair_id=" << r.repair_id
                     << " type=" << r.type
                     << " target_frame_ts="
                     << (r.target_frame_ts.has_value()
                             ? std::to_string(*r.target_frame_ts)
                             : "0")
                     << " target_seqs="
                     << (!target_seqs.empty() ? target_seqs : "-")
                     << " target_seq_count=" << r.target_seq_count
                     << " repair_packet_seqs=" << repair_seqs
                     << " repair_packet_count=" << r.repair_packet_count
                     << " nack_request_ms=" << r.nack_request_ms
                     << " repair_first_send_ms=" << r.repair_first_send_ms
                     << " repair_last_send_ms=" << r.repair_last_send_ms
                     << " repair_first_recv_ms=" << r.repair_first_recv_ms
                     << " repair_last_recv_ms=" << r.repair_last_recv_ms
                     << " duration_ms=" << r.duration_ms
                     << " recovered_seqs="
                     << (!recovered_seqs.empty() ? recovered_seqs : "-")
                     << " recovered_seq_count=" << r.recovered_seq_count
                     << " too_late_seqs="
                     << (!too_late_seqs.empty() ? too_late_seqs : "-")
                     << " too_late_seq_count=" << r.too_late_seq_count
                     << " failed_seqs="
                     << (!failed_seqs.empty() ? failed_seqs : "-")
                     << " failed_seq_count=" << r.failed_seq_count
                     << " outcome=" << r.outcome
                     ;
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
}

}  // namespace webrtc
