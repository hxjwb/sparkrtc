/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "video/twcc_time_correlator.h"

#include "modules/rtp_rtcp/source/rtp_rtcp_impl2.h"

namespace webrtc {

TwccTimeCorrelator::TwccTimeCorrelator() {
}

TwccTimeCorrelator::~TwccTimeCorrelator() {
}

void TwccTimeCorrelator::AddPacketInfo(uint16_t sequence_number, int64_t send_time_us, int64_t receive_time_us) {
  PacketTimeInfo info;
  info.sequence_number = sequence_number;
  info.send_time_us = send_time_us;
  info.receive_time_us = receive_time_us;
  
  packet_times_[sequence_number] = info;
  MaintainHistorySize();
}

void TwccTimeCorrelator::AddRtpTimestampMapping(uint16_t sequence_number, uint32_t rtp_timestamp) {
  rtp_timestamp_mapping_[sequence_number] = rtp_timestamp;
  MaintainHistorySize();
}

std::vector<PacketTimeInfo> TwccTimeCorrelator::GetPacketTimesForFrame(uint32_t rtp_timestamp) const {
  std::vector<PacketTimeInfo> times;
  
  // Find all packets with the given RTP timestamp
  for (const auto& entry : rtp_timestamp_mapping_) {
    if (entry.second == rtp_timestamp) {
      auto packet_it = packet_times_.find(entry.first);
      if (packet_it != packet_times_.end()) {
        times.push_back(packet_it->second);
      }
    }
  }
  
  return times;
}

void TwccTimeCorrelator::MaintainHistorySize() {
  while (packet_times_.size() > max_packet_history_) {
    uint16_t oldest_seq = packet_times_.begin()->first;
    packet_times_.erase(packet_times_.begin());
    rtp_timestamp_mapping_.erase(oldest_seq);
  }
}

}  // namespace webrtc
