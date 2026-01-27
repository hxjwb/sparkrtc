yuvPath=/mnt/md3/xiangjie/youtubevideos/evaluation_video/bathsong_coded.yuv

# single test
python3 kill_all.py
python3 run.py --file $yuvPath
python3 get_stall_windows.py --out-dir cases/case_2/session_1
#  python prfl.py logs > logs/p.lo


# # python3 mahi_serial.py logs
# testName='121_'$(date +%Y%m%d_%H%M%S)
# # create if not exist
# mkdir -p archive/$testName
# cp logs/* archive/$testName/
# python3 mahi_serial.py archive/$testName/


