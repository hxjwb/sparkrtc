/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef VIDEO_TWCC_TIME_CORRELATOR_H_
#define VIDEO_TWCC_TIME_CORRELATOR_H_

#include <map>
#include <vector>

#include "absl/types/optional.h"
#include "api/units/timestamp.h"
#include "modules/include/module_common_types_public.h"

namespace webrtc {

struct PacketTimeInfo {
  uint16_t sequence_number;
  int64_t send_time_us;
  int64_t receive_time_us;
};

class TwccTimeCorrelator {
 public:
  TwccTimeCorrelator();
  ~TwccTimeCorrelator();

  void AddPacketInfo(uint16_t sequence_number, int64_t send_time_us, int64_t receive_time_us);
  void AddRtpTimestampMapping(uint16_t sequence_number, uint32_t rtp_timestamp);
  std::vector<PacketTimeInfo> GetPacketTimesForFrame(uint32_t rtp_timestamp) const;
  absl::optional<PacketTimeInfo> GetPacketTime(uint16_t sequence_number) const;
  
 private:
  struct SequenceNumberOlderThan {
    bool operator()(uint16_t seq1, uint16_t seq2) const {
      return IsNewerSequenceNumber(seq2, seq1);
    }
  };
  
  std::map<uint16_t, PacketTimeInfo, SequenceNumberOlderThan> packet_times_;
  std::map<uint16_t, uint32_t, SequenceNumberOlderThan> rtp_timestamp_mapping_;
  size_t max_packet_history_ = 2000;

  void MaintainHistorySize();
};

}  // namespace webrtc

#endif  // VIDEO_TWCC_TIME_CORRELATOR_H_
