
bin_path = "/home/xiangjie/sparkrtc/out/t"
# video_file = "/home/xiangjie/Mahimahi-Test/video/gta6_30_coded.yuv"
w,h = 1920, 1080
fps = 30
# recon_file = "res/recon.yuv"
duration =  5 # seconds


import subprocess
import os
import time

def start_process(cmd, error_log_file=None, std_log_file=None):
    if error_log_file:
        with open(error_log_file, 'w') as f:
            return subprocess.Popen(cmd, shell=True,stdin=subprocess.PIPE, stderr=f, preexec_fn=os.setsid)
    else:
        return subprocess.Popen(cmd, shell=True,preexec_fn=os.setsid)

def kill_process(process):
    import signal
    process.terminate() 
    process.wait()
    os.killpg(process.pid,signal.SIGTERM)

# queue_size = 1
if __name__ == "__main__":
    
    
    import json 
    import argparse
    import os
    os.system("sudo sysctl -w net.ipv4.ip_forward=1") # required by mm-link
    parser = argparse.ArgumentParser()
    parser.add_argument('--file', type=str, default="config.json")
    video_file = parser.parse_args().file
    
    trace_up = "/mnt/md3/xiangjie/Mahimahi-Test/traces/24mbps.t"
    # trace_down = "/mnt/md3/xiangjie/ACE_evaluation/mahi_trace/rest-wifi_0.pitree-trace"
    trace_down = "/mnt/md3/xiangjie/Mahimahi-Test/traces/24mbps.t"
    
    queue_size = 640
    # logsend = "log_send"
    # logrecv = "log_recv"
    recon_file = "recon/recon.yuv"
    
    
    
    # end process that occupies the port 8989
    
    # delete logs/mah.log
    os.system("rm -f logs/mah.log")
    
    os.system("fuser -k 8989/tcp")

    # Start the server
    server_cmd = os.path.join(bin_path, 'peerconnection_server --port 8989')
    server_process = start_process(server_cmd)
    
    time.sleep(1)
    
    # Start the clients
    cmd_sender = f"{bin_path}/peerconnection_localvideo --file {video_file} --width {w} --height {h} --fps {fps} --logname send --port 8989 2>logs/send_stderr.log "
    

    cmd_receiver = f"mm-link {trace_up} {trace_down} --downlink-log=logs/mah.log --downlink-queue=droptail --downlink-queue-args=packets={queue_size}"
    # cmd_receiver = "mm-lo dss downlink 0.1"
    # cmd_receiver = "mm-delay 1000"
    rec_process = start_process(cmd_receiver, 'logs/receiver_process_debug.log')
    
    input_line = f"ifconfig > ifconfig.txt\n" 
    rec_process.stdin.write(input_line.encode())
    rec_process.stdin.flush()
    
    # # read ifconfig.txt to get the ip address
    # with open("ifconfig.txt", "r") as f:
    #     lines = f.readlines()
    #     for line in lines:
    #         if "inet" in line and "
    
    input_line = f"{bin_path}/peerconnection_localvideo --recon {recon_file} --server 100.64.0.1 --logname recv --port 8989 2> logs/recv_stderr.log &\n" # 
    print(input_line)
    rec_process.stdin.write(input_line.encode())
    rec_process.stdin.flush()
    
    

    # # receiver first, wait for 200ms
    time.sleep(1)
    sen_process = start_process(cmd_sender, 'logs/sender_process_debug.log')

    time.sleep(3)
    
    # add compete stream
    # COMPETE_FLOW = True
    # if COMPETE_FLOW:
        
    #     input_line = f"python3 sel.py \n"
        
    #     rec_process.stdin.write(input_line.encode())
    #     rec_process.stdin.flush()
    
    time.sleep(duration)
    
    kill_process(server_process)
    # wait for sender to finish
    # sen_process.wait()
    
    
    
    # kill receiver and server processes
    kill_process(rec_process)
    kill_process(server_process)

    print("Test Done!")
    
    # copy logs to /archive/test_name
    # archive = '/mnt/md3/xiangjie/EVA/archive'
    # os.system(f"mkdir -p {archive}/{test_name}")
    # # create a folder of the timestamp
    # timestamp = time.strftime("%Y-%m-%d-%H-%M-%S", time.localtime())
    # os.system(f"mkdir -p {archive}/{test_name}/{timestamp}")
    # os.system(f"cp logs/log_recv_0 {archive}/{test_name}/{timestamp}/")
    # os.system(f"cp logs/log_send_0 {archive}/{test_name}/{timestamp}/")
    # os.system(f"cp logs/mah.log {archive}/{test_name}/{timestamp}/")
    # os.system(f"cp logs/opentime.json {archive}/{test_name}/{timestamp}/")
    # # copy recon yuv to /archive/test_name
    # os.system(f"cp {recon_file} {archive}/{test_name}/{timestamp}/")
    
    # remove {recon_file}

    # os.system(f"x264 --input-res {w}x{h} --fps {fps} --preset ultrafast --crf 15 {recon_file} -o archive/{test_name}/{timestamp}/recon_archive.bin")
    # os.system(f"rm -f {recon_file}")
    
    
    # os.system(f'python3 neweva.py --path archive/{test_name}/{timestamp}')
    
    