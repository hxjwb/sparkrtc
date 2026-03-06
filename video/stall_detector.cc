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
  const size_t history_size = decode_history_.size();
  delays.reserve(std::min(count, history_size));

  size_t start_idx = history_size > count ? history_size - count : 0;
  // if (history_size >= 2) {
  //   std::vector<int64_t> gaps_us;
  //   gaps_us.reserve(history_size - 1);
  //   for (size_t i = 1; i < history_size; ++i) {
  //     const int64_t gap = decode_history_[i].arrival_time_us -
  //                         decode_history_[i - 1].arrival_time_us;
  //     if (gap > 0) {
  //       gaps_us.push_back(gap);
  //     }
  //   }
  //   if (!gaps_us.empty()) {
  //     std::nth_element(gaps_us.begin(),
  //                      gaps_us.begin() + gaps_us.size() / 2,
  //                      gaps_us.end());
  //     const int64_t median_gap_us = gaps_us[gaps_us.size() / 2];
  //     const int64_t threshold_us = std::max<int64_t>(
  //         median_gap_us * 13 / 10, median_gap_us + 2000);
  //     size_t stall_idx = history_size;
  //     if (last_stall_rtp_timestamp_ != 0) {
  //       for (size_t i = history_size; i-- > 0;) {
  //         if (decode_history_[i].rtp_timestamp == last_stall_rtp_timestamp_) {
  //           stall_idx = i;
  //           break;
  //         }
  //       }
  //     }
  //     if (stall_idx < history_size && stall_idx > 0) {
  //       size_t normal_idx = stall_idx;
  //       while (normal_idx > 0) {
  //         const int64_t gap = decode_history_[normal_idx].arrival_time_us -
  //                             decode_history_[normal_idx - 1].arrival_time_us;
  //         if (gap <= threshold_us) {
  //           break;
  //         }
  //         --normal_idx;
  //       }
  //       constexpr size_t kLookbackFrames = 2;
  //       const size_t adaptive_start =
  //           (normal_idx > kLookbackFrames) ? (normal_idx - kLookbackFrames) : 0;
  //       if (adaptive_start > start_idx) {
  //         start_idx = adaptive_start;
  //       }
  //     }
  //   }
  // }
  for (size_t i = start_idx; i < history_size; ++i) {
    const auto& info = decode_history_[i];
    DecodeDelayInfo delay;
    delay.rtp_timestamp = info.rtp_timestamp;
    delay.decode_delay_us = info.decode_time_us;
    delay.assemble_to_decode_us = info.assemble_to_decode_us;
    delay.decode_end_time_us = info.arrival_time_us;
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
