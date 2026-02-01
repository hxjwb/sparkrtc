#ifndef EXAMPLES_LOCALVIDEO_CAPTURE_LOCALVIDEO_CAPTURER_SOURCE_TEST_H_
#define EXAMPLES_LOCALVIDEO_CAPTURE_LOCALVIDEO_CAPTURER_SOURCE_TEST_H_

#include "api/video/video_frame.h"
#include "api/video_track_source_constraints.h"
#include "api/video/video_source_interface.h"
#include "media/base/video_adapter.h"
#include "media/base/video_broadcaster.h"

namespace webrtc {

class TestDesktopCapturer
    : public rtc::VideoSourceInterface<webrtc::VideoFrame> {
 public:
  TestDesktopCapturer() {}
  ~TestDesktopCapturer() override {}

  void ProcessConstraints(
      const webrtc::VideoTrackSourceConstraints& constraints);

  void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                       const rtc::VideoSinkWants& wants) override;

  void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override;

 protected:
  // Notify sinkes
  void OnFrame(const webrtc::VideoFrame& frame);

 private:
  void UpdateVideoAdapter();

  rtc::VideoBroadcaster broadcaster_;
  cricket::VideoAdapter video_adapter_;
};

}  // namespace webrtc

#endif  // EXAMPLES_LOCALVIDEO_CAPTURE_LOCALVIDEO_CAPTURER_SOURCE_TEST_H_