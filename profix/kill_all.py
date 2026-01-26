import psutil

def kill_ffmpeg_processes():
    # list all the command of the processes
    for proc in psutil.process_iter(['pid', 'name', 'cmdline']):
        # print(proc.info)
        if proc.info['cmdline'] is None:
            continue
        
        cmdline = ' '.join(proc.info['cmdline'])
        if 'ffmpeg' in cmdline or 'peerconnection' in cmdline or 'salsify' in cmdline:
            try:
                print(proc.info)
                print(f"Killing {proc.info['name']} with PID {proc.info['pid']}")
                proc.kill()
            except:
                print(f"Failed to kill {proc.info['name']} with PID {proc.info['pid']}")
                


if __name__ == "__main__":
    kill_ffmpeg_processes()