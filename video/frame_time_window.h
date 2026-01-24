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
#include <vector>

#include "api/units/timestamp.h"

namespace webrtc {

class TwccTimeCorrelator;

struct PacketTimingInfo {
  uint16_t rtp_sequence_number;
  uint16_t transport_sequence_number;
  int64_t send_time_ms;
};

struct FrameTimingInfo {
  uint32_t rtp_timestamp;
  int64_t encode_start_time_ms;
  int64_t encode_end_time_ms;
  std::vector<PacketTimingInfo> media_packets;
  std::vector<PacketTimingInfo> retrans_packets;
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
                int64_t encode_end_time_ms);
  void AddMediaPacket(uint32_t rtp_timestamp,
                      uint16_t rtp_seq,
                      uint16_t transport_seq,
                      int64_t send_time_ms);
  void AddRetransPacket(uint32_t rtp_timestamp,
                        uint16_t rtp_seq,
                        uint16_t transport_seq,
                        int64_t send_time_ms);
  
  void PrintProfilingInfo(const std::vector<DecodeDelayInfo>& decode_delays, TwccTimeCorrelator* twcc_correlator = nullptr);

 private:
  const size_t window_size_;
  std::deque<FrameTimingInfo> frame_window_;

  FrameTimingInfo* FindFrameByTimestamp(uint32_t rtp_timestamp);
  void MaintainWindowSize();
};

}  // namespace webrtc

#endif  // VIDEO_FRAME_TIME_WINDOW_H_
