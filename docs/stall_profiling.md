## Stall Profiling

This feature records per-frame and per-packet timing around receiver stalls,
and prints a timeline on the sender when a stall is detected.

### What it does

- Sender keeps a 100-frame window of encode timings and packet send times.
- Receiver detects stalls based on decode gaps
  (default threshold: 100ms) and sends a custom RTCP APP
  packet (`STLL`) carrying:
  - the stall frame RTP timestamp
  - decode time per frame (last 30 frames)
  - assemble-to-decode time per frame (last 30 frames)
- Sender receives the report, correlates it with the frame window, and uses
  TWCC feedback to map transport sequence numbers to receive timestamps.
- Sender prints a combined frame + packet timeline.

### Data flow

1. **Sender**
   - `FrameTimeWindow` stores encode start/end and packet send times.
   - `RtpSenderEgress` records send time, RTP seq, transport seq per packet.
   - `RtpTransportControllerSend` stores TWCC send/recv timestamps in
     `TwccTimeCorrelator`.
2. **Receiver**
   - `StallDetector` monitors decode intervals. On stall, it waits 10 frames
     and then reports the latest 30 frames.
   - `VideoReceiveStream2` builds RTCP APP `STLL` and sends it.
3. **Sender**
   - `RTCPReceiver` parses `STLL` and forwards to `ModuleRtpRtcpImpl2`.
   - `ModuleRtpRtcpImpl2` calls `FrameTimeWindow::PrintProfilingInfo(...)`.

### RTCP APP format (`STLL`)

All values are network byte order:

- `stall_rtp_timestamp` (4 bytes)
- `report_size_bytes` (4 bytes, version 4)
- Repeated for each frame (28 bytes per entry):
  - `frame_rtp_timestamp` (4 bytes)
  - `decode_delay_us` (8 bytes)
  - `assemble_to_decode_us` (8 bytes)
  - `decode_end_time_us` (8 bytes)

### Sender log format

The sender prints both human-readable logs and a parse-friendly format:

- Stall marker:
  - `PRFL_STALL rtp_ts=<rtp_timestamp>`
- Per-frame:
  - `PRFL_FRAME rtp_ts=... enc_start_ms=... enc_end_ms=... enc_time_ms=...`
  - `first_send_ms=... enc_to_first_send_ms=... last_recv_ms=...`
  - `last_recv_to_decode_ms=... decode_time_ms=...`
- Per-packet:
  - `PRFL_PKT rtp_ts=... kind=media|retrans rtp_seq=... transport_seq=...`
  - `send_ms=... recv_ms=... delay_ms=...`

### Plotting

Use the helper script to draw the timeline:

```
python3 profix/plot_profile.py --log profix/logs/send_0 \
  --out profix/logs/profile_timeline.png
```

Options:

- `--split`: one image per stall report
- `--max-pkts-per-frame N`: limit packet points per frame
- `--no-pkts`: skip packet points

### Notes

- RTP timestamps are used to correlate frames between sender and receiver.
- TWCC transport sequence numbers are used to map send/receive packet times.
- All timeline values are printed in milliseconds.
