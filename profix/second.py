import re
import argparse
import os
import json


def parse_b_log(log_path):
    time_sync = None
    frames = []
    pattern_sync = re.compile(r"^Time sync:\s*(\d+)\s*ms")
    pattern_frame = re.compile(
        r"^RTP TS:\s*(\d+),\s*Frame Size:\s*(\d+),\s*Encode\s*(\d+),\s*Net\s*(\d+),\s*Wait\s*(\d+),\s*Decode\s*(\d+)\s*E2E\s*(\d+)\s*$"
    )
    pattern_packet = re.compile(
        r"^P:\s*seq\s*(\d+),\s*size\s*(\d+),\s*send_delta\s*(\d+),\s*trans_delta\s*(\d+)\s*$"
    )

    current = None

    def finalize_current(curr):
        if curr is None:
            return
        send_span = None
        last_trans_delay = None
        first_send_delta = None
        if curr.get("p_send_deltas"):
            # 从首包到末包的发送时间跨度
            first_send_delta = curr["p_send_deltas"][0]
            send_span = curr["p_send_deltas"][ -1 ] - first_send_delta
        if curr.get("p_trans_deltas"):
            # 末包的传输延迟
            last_trans_delay = curr["p_trans_deltas"][ -1 ]

        frames.append(
            {
                "rtp_ts": curr["rtp_ts"],
                "frame_size": curr["frame_size"],
                "encode": curr["encode"],
                # 拆分 net 为两个字段
                "first_send_delta": first_send_delta,
                "send_span": send_span,
                "last_trans_delay": last_trans_delay,
                "wait": curr["wait"],
                "decode": curr["decode"],
                "e2e": curr["e2e"],
            }
        )

    with open(log_path, "r") as f:
        for raw in f:
            line = raw.strip()

            m_sync = pattern_sync.match(line)
            if m_sync:
                time_sync = int(m_sync.group(1))
                continue

            m_frame = pattern_frame.match(line)
            if m_frame:
                # 完结上一个帧
                finalize_current(current)

                # 开始新帧
                current = {
                    "rtp_ts": int(m_frame.group(1)),
                    "frame_size": int(m_frame.group(2)),
                    "encode": int(m_frame.group(3)),
                    # 原始 net 值读取但不输出（已拆分）
                    "net": int(m_frame.group(4)),
                    "wait": int(m_frame.group(5)),
                    "decode": int(m_frame.group(6)),
                    "e2e": int(m_frame.group(7)),
                    "p_send_deltas": [],
                    "p_trans_deltas": [],
                }
                continue

            m_packet = pattern_packet.match(line)
            if m_packet and current is not None:
                send_delta = int(m_packet.group(3))
                trans_delta = int(m_packet.group(4))
                current["p_send_deltas"].append(send_delta)
                current["p_trans_deltas"].append(trans_delta)
                continue

        # 文件结束，收尾最后一帧
        finalize_current(current)

    return time_sync, frames


def save_outputs(frames, out_dir, base_name="frame_delays"):
    os.makedirs(out_dir, exist_ok=True)
    json_path = os.path.join(out_dir, f"{base_name}.json")
    csv_path = os.path.join(out_dir, f"{base_name}.csv")

    with open(json_path, "w") as jf:
        json.dump(frames, jf, indent=2)

    with open(csv_path, "w") as cf:
        cf.write("rtp_ts,frame_size,encode,first_send_delta,send_span,last_trans_delay,wait,decode,e2e\n")
        for fr in frames:
            first_send = "" if fr["first_send_delta"] is None else fr["first_send_delta"]
            send_span = "" if fr["send_span"] is None else fr["send_span"]
            last_trans = "" if fr["last_trans_delay"] is None else fr["last_trans_delay"]
            cf.write(
                f"{fr['rtp_ts']},{fr['frame_size']},{fr['encode']},{first_send},{send_span},{last_trans},{fr['wait']},{fr['decode']},{fr['e2e']}\n"
            )

    return json_path, csv_path


def main():
    parser = argparse.ArgumentParser(description="Parse logs/b.log to extract per-frame delay components")
    parser.add_argument("--log", type=str, default="logs/b.log", help="Path to b.log")
    parser.add_argument(
        "--outdir", type=str, default="logs", help="Directory to write outputs (CSV/JSON)"
    )
    args = parser.parse_args()

    tsync, frames = parse_b_log(args.log)

    if tsync is not None:
        print(f"Time sync: {tsync} ms")

    print("rtp_ts, frame_size, encode, first_send_delta, send_span, last_trans_delay, wait, decode, e2e")
    for fr in frames:
        first_send = "" if fr["first_send_delta"] is None else fr["first_send_delta"]
        send_span = "" if fr["send_span"] is None else fr["send_span"]
        last_trans = "" if fr["last_trans_delay"] is None else fr["last_trans_delay"]
        print(
            f"{fr['rtp_ts']}, {fr['frame_size']}, {fr['encode']}, {first_send}, {send_span}, {last_trans}, {fr['wait']}, {fr['decode']}, {fr['e2e']}"
        )

    json_path, csv_path = save_outputs(frames, args.outdir)
    print(f"Saved: {json_path}")
    print(f"Saved: {csv_path}")

    # High-latency event analysis: aggregate component delays
    components = {
        "encoder": 0,
        "send_queue": 0,  # first_send_delta + send_span
        "network": 0,     # last_trans_delay
        "recv_queue": 0,  # wait
        "decoder": 0,
    }

    # Track maxima for supporting details
    maxima = {k: 0 for k in components}
    counts = 0
    for fr in frames:
        enc = fr.get("encode", 0) or 0
        fs = fr.get("first_send_delta", 0) or 0
        ss = fr.get("send_span", 0) or 0
        net = fr.get("last_trans_delay", 0) or 0
        wait = fr.get("wait", 0) or 0
        dec = fr.get("decode", 0) or 0

        components["encoder"] += enc
        components["send_queue"] += (fs + ss)
        components["network"] += net
        components["recv_queue"] += wait
        components["decoder"] += dec

        maxima["encoder"] = max(maxima["encoder"], enc)
        maxima["send_queue"] = max(maxima["send_queue"], fs + ss)
        maxima["network"] = max(maxima["network"], net)
        maxima["recv_queue"] = max(maxima["recv_queue"], wait)
        maxima["decoder"] = max(maxima["decoder"], dec)
        counts += 1

    # Determine dominant component by total contribution
    dominant = max(components.items(), key=lambda x: x[1])
    name, total = dominant
    print("\nHigh-latency event analysis (aggregate across frames):")
    print("Components total delay (ms):")
    for k, v in components.items():
        avg = v / counts if counts else 0
        print(f"  {k}: total={v}, avg={avg:.2f}, max={maxima[k]}")
    print(f"\nDominant component: {name}")

    if name == "send_queue":
        name == "pacing_queue on the sender side"
        code = "pacing_controller.cc"
    else:
        code = "None"
    prompt = f'''
            I found that {name} is the component to be blame for high latency. Can you modify the codes to be adaptive to reduce latency?

            Please go with these files:
            {code}

            first, and modify other necessary files to make it bug free. Do not modify too much codes. Please modify the codes before thinking too much. Please priortize modify code logic rather than adjusting parameter."
            '''
    print(prompt)

if __name__ == "__main__":
    main()