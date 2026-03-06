git add -A
git commit -m "chore: 提交当前改动"
git push
ssh eez221 "cd sparkrtc && git pull"
ssh eez221 "cd sparkrtc && ninja -C out/t"
