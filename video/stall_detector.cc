/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "video/stall_detector.h"

#include <algorithm>

#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace webrtc {

StallDetector::StallDetector(StallDetectorObserver* observer,
                             int64_t stall_threshold_us)
    : stall_threshold_us_(stall_threshold_us), observer_(observer) {}

StallDetector::~StallDetector() {}

void StallDetector::SetObserver(StallDetectorObserver* observer) {
  observer_ = observer;
}

void StallDetector::OnFrameDecoded(uint32_t rtp_timestamp,
                                   int64_t decode_time_us,
                                   int64_t assemble_to_decode_us) {
  FrameDecodeInfo info;
  info.rtp_timestamp = rtp_timestamp;
  info.decode_time_us = decode_time_us;
  info.assemble_to_decode_us = assemble_to_decode_us;
  info.arrival_time_us = rtc::TimeMicros();
  
  decode_history_.push_back(info);
  MaintainHistorySize();
  
  if (stall_pending_) {
    --pending_frames_remaining_;
    if (pending_frames_remaining_ <= 0) {
      CheckForStall();
      stall_pending_ = false;
      pending_frames_remaining_ = 0;
    }
  } else if (last_decode_time_us_ != -1) {
    int64_t delta_us = info.arrival_time_us - last_decode_time_us_;
    if (delta_us > stall_threshold_us_) {
      RTC_LOG(LS_INFO) << "Stall detected: delta_us = " << delta_us;
      stall_pending_ = true;
      pending_frames_remaining_ = 1;
      last_stall_rtp_timestamp_ = info.rtp_timestamp;
      last_stall_gap_us_ = delta_us;
    }
  }
  
  last_decode_time_us_ = info.arrival_time_us;
}

std::vector<DecodeDelayInfo> StallDetector::GetRecentDecodeDelays(size_t count) const {
  std::vector<DecodeDelayInfo> delays;
  delays.reserve(std::min(count, decode_history_.size()));
  
  size_t start_idx = decode_history_.size() > count ? decode_history_.size() - count : 0;
  for (size_t i = start_idx; i < decode_history_.size(); ++i) {
    const auto& info = decode_history_[i];
    DecodeDelayInfo delay;
    delay.rtp_timestamp = info.rtp_timestamp;
    delay.decode_delay_us = info.decode_time_us;
    delay.assemble_to_decode_us = info.assemble_to_decode_us;
    delays.push_back(delay);
  }
  
  return delays;
}

void StallDetector::CheckForStall() {
  if (observer_) {
    std::vector<DecodeDelayInfo> delays = GetRecentDecodeDelays(30);
    observer_->OnStallDetected(delays, last_stall_rtp_timestamp_,
                               last_stall_gap_us_ / 1000);
  }
}

void StallDetector::MaintainHistorySize() {
  const size_t max_history_size = 100;  // 保持足够的历史记录
  while (decode_history_.size() > max_history_size) {
    decode_history_.pop_front();
  }
}

}  // namespace webrtc
