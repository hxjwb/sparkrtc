git add -A
git commit -m "chore: 提交当前改动"
git push
ssh eez221.ece.ust.hk "cd sparkrtc && git pull"
ssh eez221.ece.ust.hk "cd sparkrtc && ninja -C out/t"
