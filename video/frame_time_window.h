/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef VIDEO_FRAME_TIME_WINDOW_H_
#define VIDEO_FRAME_TIME_WINDOW_H_

#include <deque>
#include <string>
#include <vector>

#include "absl/types/optional.h"
#include "api/units/timestamp.h"

namespace webrtc {

class TwccTimeCorrelator;

enum class PacketKind {
  kMedia,
  kRtx,
  kFec,
};

struct PacketTimingInfo {
  uint16_t rtp_sequence_number;
  uint16_t transport_sequence_number;
  int64_t send_time_ms;
  size_t size_bytes;
  PacketKind kind;
  absl::optional<uint16_t> rtx_target_seq;
  absl::optional<uint32_t> fec_repair_id;
};

struct FrameTimingInfo {
  uint32_t rtp_timestamp;
  int64_t encode_start_time_ms;
  int64_t encode_end_time_ms;
  size_t frame_size_bytes = 0;
  bool is_keyframe = false;
  std::vector<PacketTimingInfo> media_packets;
  std::vector<PacketTimingInfo> retrans_packets;
};

struct FecPacketInfo {
  uint32_t repair_id;
  PacketTimingInfo packet;
  std::vector<uint16_t> protected_sequence_numbers;
};

struct StallReportHeader {
  std::string codec;
  int width = -1;
  int height = -1;
  int fps_nominal = -1;
  bool rtx_enabled = false;
  bool fec_enabled = false;
};

struct DecodeDelayInfo {
  uint32_t rtp_timestamp;
  int64_t decode_delay_us;
  int64_t assemble_to_decode_us;
};

class FrameTimeWindow {
 public:
  explicit FrameTimeWindow(size_t window_size = 100);
  ~FrameTimeWindow();

  void AddFrame(uint32_t rtp_timestamp,
                int64_t encode_start_time_ms,
                int64_t encode_end_time_ms,
                size_t frame_size_bytes,
                bool is_keyframe);
  void AddMediaPacket(uint32_t rtp_timestamp,
                      uint16_t rtp_seq,
                      uint16_t transport_seq,
                      int64_t send_time_ms,
                      size_t size_bytes);
  void AddRetransPacket(uint32_t rtp_timestamp,
                        uint16_t rtp_seq,
                        uint16_t transport_seq,
                        int64_t send_time_ms,
                        size_t size_bytes,
                        absl::optional<uint16_t> rtx_target_seq);
  void AddFecPacket(uint16_t rtp_seq,
                    uint16_t transport_seq,
                    int64_t send_time_ms,
                    size_t size_bytes,
                    const std::vector<uint16_t>& protected_sequence_numbers);

  void SetCodecName(const std::string& codec);
  void SetVideoDimensions(int width, int height);
  void SetFpsNominal(int fps);
  void SetRtxFecEnabled(bool rtx_enabled, bool fec_enabled);
  
  void PrintProfilingInfo(const std::vector<DecodeDelayInfo>& decode_delays,
                          uint32_t stall_rtp_timestamp,
                          int64_t stall_gap_ms,
                          uint32_t nack_sent,
                          TwccTimeCorrelator* twcc_correlator = nullptr);

 private:
  const size_t window_size_;
  std::deque<FrameTimingInfo> frame_window_;
  std::deque<FecPacketInfo> fec_packet_window_;
  std::deque<PacketTimingInfo> rtx_packet_window_;
  StallReportHeader report_header_;
  uint32_t next_repair_id_ = 1;

  FrameTimingInfo* FindFrameByTimestamp(uint32_t rtp_timestamp);
  void MaintainWindowSize();
  uint32_t AllocateRepairId();
};

}  // namespace webrtc

#endif  // VIDEO_FRAME_TIME_WINDOW_H_
