ssh eez221.ece.ust.hk "cd sparkrtc/profix && bash run.sh"
scp eez221.ece.ust.hk:sparkrtc/profix/logs/send_0 logs/
scp eez221.ece.ust.hk:sparkrtc/profix/logs/recv_0 logs/
scp eez221.ece.ust.hk:sparkrtc/profix/logs/mah.log logs/
ssh eez221.ece.ust.hk "cd sparkrtc/profix && python3 prfl.py logs > logs/p.log"
scp eez221.ece.ust.hk:sparkrtc/profix/logs/p.log logs/
python3 /Users/bytedance/sparkrtc/plot_bwe.py --mah-log /Users/bytedance/sparkrtc/logs/mah.log --send-log /Users/bytedance/sparkrtc/logs/send_0 --p-log /Users/bytedance/sparkrtc/logs/p.log --out /Users/bytedance/sparkrtc/logs/bwe_plot.html