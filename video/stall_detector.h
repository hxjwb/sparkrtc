/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef VIDEO_STALL_DETECTOR_H_
#define VIDEO_STALL_DETECTOR_H_

#include <deque>
#include <vector>

#include "api/units/timestamp.h"
#include "video/frame_time_window.h"

namespace webrtc {

class StallDetectorObserver {
 public:
  virtual ~StallDetectorObserver() = default;
  virtual void OnStallDetected(const std::vector<DecodeDelayInfo>& decode_delays,
                               uint32_t stall_rtp_timestamp,
                               int64_t stall_gap_ms) = 0;
};

class StallDetector {
 public:
  explicit StallDetector(StallDetectorObserver* observer,
                         int64_t stall_threshold_us = 50000);  // 100ms
  ~StallDetector();

  void SetObserver(StallDetectorObserver* observer);
  void OnFrameDecoded(uint32_t rtp_timestamp,
                      int64_t decode_time_us,
                      int64_t assemble_to_decode_us);
  
  std::vector<DecodeDelayInfo> GetRecentDecodeDelays(size_t count = 30) const;

 private:
  const int64_t stall_threshold_us_;
  StallDetectorObserver* observer_ = nullptr;
  
  struct FrameDecodeInfo {
    uint32_t rtp_timestamp;
    int64_t decode_time_us;
    int64_t assemble_to_decode_us;
    int64_t arrival_time_us;
  };
  
  std::deque<FrameDecodeInfo> decode_history_;
  int64_t last_decode_time_us_ = -1;
  bool stall_pending_ = false;
  int pending_frames_remaining_ = 0;
  uint32_t last_stall_rtp_timestamp_ = 0;
  int64_t last_stall_gap_us_ = 0;

  void CheckForStall();
  void MaintainHistorySize();
};

}  // namespace webrtc

#endif  // VIDEO_STALL_DETECTOR_H_
