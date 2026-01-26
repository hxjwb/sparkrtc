file1 = '/home/xiangjie/profile/profix/archive/114_20260114_200208/b copy.log'

file2 = '/home/xiangjie/profile/profix/archive/114_20260114_200208/send_0'

lines1 = open(file1).readlines()


seq2delay = {}
for line in lines1:
    if 'P: seq ' in line:
        # P: seq 39037, size 1168, send_delta 95, trans_delta 148
        seq = int(line.split('seq ')[1].split(',')[0])
        size = int(line.split('size ')[1].split(',')[0])
        send_delta = int(line.split('send_delta ')[1].split(',')[0])
        try:
            trans_delta = int(line.split('trans_delta ')[1].split(',')[0])
        except:
            trans_delta = -1
        if seq != 0 and trans_delta != -1:
            seq2delay[seq] = trans_delta
            
lines2 = open(file2).readlines()

seq2delay2 = {}
for line in lines2:
    if 'Packet Delay - RTP Seq:' in line:
        # Packet Delay - RTP Seq: 28122 Send: 778130854ms Recv: 778130909ms Delay: 55ms
        seq = int(line.split('RTP Seq:')[1].split('Send:')[0].strip())
        delay = int(line.split('Delay:')[1].split('ms')[0].strip())
        seq2delay2[seq] = delay

import matplotlib.pyplot as plt

# 获取两个 dict 中共同的 seq
common_seq = sorted(set(seq2delay.keys()) & set(seq2delay2.keys()))

delay1 = [seq2delay[s] + 54 for s in common_seq]
delay2 = [seq2delay2[s] for s in common_seq]

plt.figure(figsize=(10, 5))
plt.plot(common_seq, delay1, label='trans_delta (file1)')
plt.plot(common_seq, delay2, label='Delay (file2)')
plt.xlabel('Seq')
plt.ylabel('Delay / ms')
plt.title('Delay 对比')
plt.legend()
plt.grid(True)
# PDF
plt.savefig('delay_comparison.pdf', bbox_inches='tight')

import numpy as np
import matplotlib.pyplot as plt

def plot_parity_and_error_cdf(gt, meas,
                             eps_bands=(0.01, 0.05),   # 画 ±1%, ±5% 误差带
                             use_log=False,            # 延迟跨度大可设 True (log-log)
                             max_points=200000,        # 点太多就下采样
                             seed=1,
                             title_left="Parity plot",
                             title_right="CDF of |relative error|"):
    gt = np.asarray(gt, dtype=float)
    meas = np.asarray(meas, dtype=float)
    assert gt.shape == meas.shape, "gt and meas must have the same shape"

    # 过滤非法值
    mask = np.isfinite(gt) & np.isfinite(meas) & (gt > 0) & (meas > 0)
    gt = gt[mask]
    meas = meas[mask]

    # 下采样避免画面糊/文件太大
    n = len(gt)
    if n > max_points:
        rng = np.random.default_rng(seed)
        idx = rng.choice(n, size=max_points, replace=False)
        gt_s = gt[idx]
        meas_s = meas[idx]
    else:
        gt_s, meas_s = gt, meas

    # 相对误差（用于右图CDF与指标）
    r = (meas - gt) / gt
    ar = np.abs(r)

    # 关键统计量
    med = np.median(ar)
    p95 = np.percentile(ar, 95)
    p99 = np.percentile(ar, 99)

    # --- 开始作图 ---
    fig, axes = plt.subplots(1, 2, figsize=(10, 4), dpi=160)

    # ===== 左：parity plot =====
    ax = axes[0]
    # 散点：用较低alpha
    ax.scatter(gt_s, meas_s, s=3, alpha=0.15, edgecolors="none")

    # y=x 参考线与误差带
    lo = min(gt.min(), meas.min())
    hi = max(gt.max(), meas.max())
    x = np.linspace(lo, hi, 300)

    ax.plot(x, x, color="black", lw=1.2, label="y=x")
    colors = ["tab:blue", "tab:orange", "tab:green", "tab:red"]
    for i, eps in enumerate(eps_bands):
        c = colors[i % len(colors)]
        ax.plot(x, (1 + eps) * x, color=c, lw=1.0, ls="--", label=f"±{eps*100:.0f}% band" if i == 0 else None)
        ax.plot(x, (1 - eps) * x, color=c, lw=1.0, ls="--")

    ax.set_xlabel("Ground truth latency")
    ax.set_ylabel("Measured latency")
    ax.set_title(title_left)

    if use_log:
        ax.set_xscale("log")
        ax.set_yscale("log")

    # 角标统计量
    text = (f"N={len(gt):,}\n"
            f"median |r|={med*100:.2f}%\n"
            f"P95 |r|={p95*100:.2f}%\n"
            f"P99 |r|={p99*100:.2f}%")
    ax.text(0.03, 0.97, text, transform=ax.transAxes,
            va="top", ha="left",
            bbox=dict(facecolor="white", alpha=0.8, edgecolor="none"))

    # 保持等比例更直观（log时不强求）
    if not use_log:
        ax.set_aspect("equal", adjustable="box")
        ax.set_xlim(lo, hi)
        ax.set_ylim(lo, hi)

    # ===== 右：|r| 的 CDF =====
    ax = axes[1]
    ar_sorted = np.sort(ar)
    y = np.arange(1, len(ar_sorted) + 1) / len(ar_sorted)
    ax.plot(ar_sorted, y, color="tab:purple", lw=1.6)

    # 标注分位点
    for val, name in [(p99, "P99")]:
        ax.axvline(val, color="gray", lw=1.0, ls="--")
        ax.text(val, 0.02, f"{name}={val*100:.2f}%",
                rotation=90, va="bottom", ha="right", color="gray")

    ax.set_xlabel(r"|relative error| = |(meas - gt)/gt|")
    ax.set_ylabel("CDF")
    ax.set_ylim(0, 1.0)
    ax.grid(True, alpha=0.3)
    ax.set_title(title_right)

    fig.tight_layout()
    return fig, axes

# rng = np.random.default_rng(0)
gt = delay1
meas = delay2

fig, axes = plot_parity_and_error_cdf(
    gt, meas,
    eps_bands=(0.01, 0.05),
    use_log=False  # 如果跨度很大（us到ms）可以改 True
)
# plt.show()
fig.savefig("parity_and_cdf.pdf", bbox_inches="tight")