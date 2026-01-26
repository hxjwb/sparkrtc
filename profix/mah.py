

# date = '2024-05-20-09-39-58'
# logfile = f'/home/xiangjie/Mahimahi-Test/archive/test_network/{date}/mah.log'

def align_timestamp_from_log(timestamp_threadid):
    timestamp = timestamp_threadid.split(']')[0].split('[')[1]
    second = timestamp.split(':')[0]
    second = int(second)
    ms = timestamp.split(':')[1]
    ms = int(ms)
    timestamp = second * 1000 + ms
    timestamp += 1002 # offset
    return timestamp


keywords = ['Tokens: ','Bucket Size: ','LOGACTION','Standing RTT: ']




names = {}
x_list = {}
y_list = {}

for keyword in keywords:
    names[keyword] = keyword.split(':')[0]
    x_list[keyword] = []
    y_list[keyword] = []

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='get the plot of the log file')
    parser.add_argument('path', type=str, help='path to log file')
    
    import os
    logpath = parser.parse_args().path
    logfile = os.path.join(logpath, 'mah.log')
    
    logsend = os.path.join(logpath, 'send_0')
    
    logrecv = os.path.join(logpath, 'recv_0')
    
    
    
    file = open(logfile, 'r')
    lines = file.readlines()
    file.close()
    
    file = open(logsend, 'r')
    send_lines = file.readlines()
    file.close()
    
    
    # legacy : get capacity from log_recv
    # file = open(logrecv, 'r')
    # recv_lines = file.readlines()
    # file.close()
    
    # dic_cap = {}
    # for line in recv_lines:
    #     if 'sendtime recvtime' in line:
    #         line = line.strip().split()
    #         timestamp_threadid = line[0] # [017:355][604373] 
    #         timestamp = align_timestamp_from_log(timestamp_threadid)
    #         sendtime = line[-2]
    #         sendtime = int(sendtime)
    #         recvtime = line[-1]
    #         recvtime = int(recvtime)
    #         if sendtime not in dic_cap:
    #             dic_cap[sendtime] = [recvtime]
    #         else:
    #             dic_cap[sendtime].append(recvtime)
    # # print(dic_cap)
    # x_cap = []
    # y_cap = []
    # smooth_bandwidth = []
    # for line in recv_lines:

    #     if 'sendtime recvtime' in line:
    #         line = line.strip().split()
    #         timestamp_threadid = line[0] # [017:355][604373] 
    #         timestamp = align_timestamp_from_log(timestamp_threadid) - 1000
    #         sendtime = line[-2]
    #         sendtime = int(sendtime)
    #         recvtime = line[-1]
    #         recvtime = int(recvtime)
            
    #         if timestamp not in x_cap:
                
    #             time_delta = dic_cap[sendtime][-1] - dic_cap[sendtime][0]
    #             if time_delta > 0:
    #                 x_cap.append(timestamp)
    #                 bandwidth = len(dic_cap[sendtime]) / time_delta # packet/ms
    #                 bandwidth_packet_per_second = bandwidth * 1000 * 12 / 15
                    
    #                 smooth_bandwidth.append(bandwidth_packet_per_second)
                    
    #                 if len(smooth_bandwidth) > 200:
    #                     smooth_bandwidth.pop(0)
    #                     bandwidth_packet_per_second = sum(smooth_bandwidth) / 200
                    
    #                 y_cap.append(bandwidth_packet_per_second)
    
    
            
    # print(len(x_cap), len(y_cap))
    
    pid_list = []
    rtc_timestamp = []
    size_list = []
    
    x_nack = []
    y_nack = []
    
    x_rtt = []
    y_rtt = []
    
    x_bwe = []
    y_bwe = []
    
    # x_cap_sen = []
    # y_cap_sen = []
    
    smooth_capacity = 0
    
    
    x_qs_rtc = []
    y_qs_rtc = []
    for line in send_lines:
        
        
        if 'PacketID' in line:
            index = send_lines.index(line)
            line = line.strip().split()
            timestamp_threadid = line[0] # [017:355][604373] 
            timestamp = align_timestamp_from_log(timestamp_threadid)
            pid = line[-2]
            pid = int(pid)
            pid_list.append(pid)
            rtc_timestamp.append(timestamp)
            
            # 在前几行中寻找LOGSEND
            while 'LOG_SEND' not in send_lines[index]:
                index -= 1
            size = send_lines[index].strip().split()[3]
            size = int(size)
            size_list.append(size)
            # print(size)
        # [006:537][604374] (rtcp_receiver.cc:1086): Incoming NACK length: 1
        if 'Incoming NACK' in line:
            line = line.strip().split()
            timestamp_threadid = line[0] # [017:355][604373]
            timestamp = align_timestamp_from_log(timestamp_threadid)
            
            x_nack.append(timestamp)
            number = line[-1]
            number = int(number)
            y_nack.append(number)
        
        if 'RTT:' in line and 'Standing' not in line:
            line = line.strip().split()
            timestamp_threadid = line[0]
            timestamp = align_timestamp_from_log(timestamp_threadid)
            x_rtt.append(timestamp)
            rtt = line[-2]
            rtt = float(rtt)
            y_rtt.append(rtt)
        
        if 'estimate_bps=' in line:
            line = line.strip().split()
            timestamp_threadid = line[0]
            timestamp = align_timestamp_from_log(timestamp_threadid)
            x_bwe.append(timestamp)
            bwe = line[-1].split('=')[-1]
            bwe = int(bwe)
            y_bwe.append(bwe / 8 / 1500 / 30)
        # lagacy : calculate capacity by this python
        # if 'Burst Capacity:' in line:
        #     line_index = send_lines.index(line)
        #     lastline = send_lines[line_index - 1]
        #     if 'Burst Duration:' in lastline:
        #         line_elements = lastline.strip().split()
        #         duration = int(line_elements[4])
        #         if duration < 4:
        #             continue
        #         # print(line_elements)
        #     # print(lastline)
        #     line = line.strip().split()
        #     timestamp_threadid = line[0]
        #     timestamp = align_timestamp_from_log(timestamp_threadid)
        #     x_cap_sen.append(timestamp)

        #     capa = line[-2]
        #     capa = int(capa) / 8 / 1500
            
        #     # smooth_capacity.append(capa)
            
        #     # window_size = 50
        #     # if len(smooth_capacity) > window_size:
        #     #     smooth_capacity.pop(0)
        #     #     capa = sum(smooth_capacity) / window_size
        #     # else:
        #     #     capa = sum(smooth_capacity) / len(smooth_capacity)
            
        #     if smooth_capacity == 0:
        #         smooth_capacity = capa
        #     else:
        #         smooth_capacity = 0.9 * smooth_capacity + 0.1 * capa
        #     y_cap_sen.append(smooth_capacity)
        if 'Predicted Queue Size: ' in line:
            line = line.strip().split()
            timestamp_threadid = line[0]
            timestamp = align_timestamp_from_log(timestamp_threadid)
            x_qs_rtc.append(timestamp)
            size = line[-1]
            size = 400 - int(size)
            y_qs_rtc.append(size)
    
    
    for line in send_lines:
        for keyword in keywords:
            if keyword in line:
                index = keywords.index(keyword)
                line = line.strip().split()
                timestamp_threadid = line[0]
                timestamp = align_timestamp_from_log(timestamp_threadid)
                # print(timestamp)
                x_list[keyword].append(timestamp)
                # print(line[-1])
                y_list[keyword].append(int(line[-1]))

                
    
    # x_qs = x_rtt
    # y_qs = []
    # for rtt_ts in x_rtt:
    #     rtt = y_rtt[x_rtt.index(rtt_ts)]
    #     # find the previous closest cap_sen
    #     temp = [i for i in x_cap_sen if i < rtt_ts]
    #     if len(temp) == 0:
    #         index = 0
    #     else:
    #         index = len(temp) - 1
    #     cap_sen = y_cap_sen[index]
    #     # print(cap_sen, rtt, cap_sen * rtt / 1000)
    #     y_qs.append(400 - cap_sen * rtt / 1000)
    
            
            
            
            

        
            
    # exit(0)


    x = []
    y = []

    dic_packets = {}
    
    dic_frames = {}
    
    dic_drop = {}
    
    dic_bandwidth = {}
    for line in lines:
        # if ' # ' in line:
    
        if ' s ' in line:
            line = line.strip().split()
            
            # print(line)
            timestamp = line[0]
            timestamp = int(timestamp)
            # if timestamp < 600000:
            x.append(timestamp)
            packets = line[2]
            packets = int(packets)
            # packets = 500 - packets
            
            y.append(packets)
        if ' + ' in line:
            timestamp,_,size,pid = line.strip().split()
            timestamp = int(timestamp)
            # if timestamp == 4900:
            #     print(line)
            size = int(size)
            pid = int(pid)
            
            if pid not in pid_list:
                continue
            
            ts_rtc = rtc_timestamp[pid_list.index(pid)]
            
            # delta = timestamp - ts_rtc
            # print(pid, timestamp, ts_rtc, delta)
            
            if pid not in dic_frames:
               
                dic_frames[pid] = [timestamp]
            else:
                if timestamp not in dic_frames[pid]:
                    dic_frames[pid].append(timestamp)
            
            
            if timestamp not in dic_packets:
                dic_packets[timestamp] = size
            else:
                dic_packets[timestamp] += size

        if ' d ' in line:
            timestamp,_,_,_,pid = line.strip().split()
            timestamp = int(timestamp)
            pid = int(pid)
            if pid not in pid_list:
                continue
            
            if timestamp not in dic_drop:
                dic_drop[timestamp] = 1
            else:
                dic_drop[timestamp] += 1
    
        if ' # ' in line:
            timestamp,_,size = line.strip().split()
            timestamp = int(timestamp)
            size = int(size)
            packet_num = size / 1504
            
            if timestamp not in dic_bandwidth:
                dic_bandwidth[timestamp] = packet_num
            else:
                dic_bandwidth[timestamp] += packet_num
            

    
    # print(dic_packets)
    x2 = [] # first timestamp of each frame
    y2 = [] # packet number
    
    for pid in dic_frames:
        timestamps = dic_frames[pid]
        total_packets = [ dic_packets[timestamp] for timestamp in timestamps]
        x2.append(timestamps[0])
        # y2.append(sum(total_packets)/1500)
        y2.append(size_list[pid_list.index(pid)]/1500)
    
    
    
    # for i in range(len(x2)):
    #     print(x2[i], y2[i])
        
        
    # x2 = dic_packets.keys()
    # y2 = dic_packets.values()
        
    # x2 = list(x2)
    # y2 = list(y2)
            
    x3 = dic_drop.keys()
    y3 = dic_drop.values()
    
    x3 = list(x3)
    y3 = list(y3)
    
    x4 = dic_bandwidth.keys()
    x4 = list(x4)
    y4 = []
    
    for xi in x4:
        counter = 0
        for i in range(100):
            if xi - i in dic_bandwidth:
                counter += dic_bandwidth[xi-i]
        y4.append(counter * 10 / 30)


    # legacy using bwe * rtt, not accurate
    x_queue_size = x_rtt
    y_queue_size = []
    for ts in x_queue_size:
        rtt = y_rtt[x_rtt.index(ts)]
        if ts in x_bwe:
            index = x_bwe.index(ts)
        else:
            # find the previous bandwidth estimation
            index = x_bwe.index([i for i in x_bwe if i < ts][-1])
        bandwidth_estimation = 600
        size = 400  - bandwidth_estimation * rtt / 1000  # packets
        
        y_queue_size.append(size)
        
    import plotly.graph_objects as go

    # Create traces
    fig = go.Figure()

    fig.add_trace(go.Scatter(x=x, y=y,
                        mode='lines',
                        name='Queueing Packets'))

    # bar x2 y2 
    fig.add_trace(go.Bar(x=x2, y=y2, name='Frame Packets', width=5))



    # bar x3 y3 
    fig.add_trace(go.Bar(x=x3, y=y3, name='Dropped Packets', width=5))
    fig.add_trace(go.Bar(x=x_nack, y=y_nack, name='NACK Packets', width=5))
    fig.add_trace(go.Scatter(x=x_rtt, y=y_rtt, mode='markers', name='RTT', marker=dict(size=10, color='red')))
    fig.add_trace(go.Scatter(x=x_bwe, y=y_bwe, mode='lines', name='Bandwidth Estimation'))
    
    # scatter x4 y4
    fig.add_trace(go.Scatter(x=x4, y=y4, mode='lines', name='Bandwidth'))

    # fig.add_trace(go.Scatter(x=x_queue_size, y=y_queue_size, mode='lines', name='Queue Size')) # legacy bwe * rtt
    # fig.add_trace(go.Scatter(x=x_cap, y=y_cap, mode='lines', name='Capacity'))
    # fig.add_trace(go.Scatter(x=x_cap_sen, y=y_cap_sen, mode='lines', name='Capacity')) # calculate by this py
    # add queue size
    # fig.add_trace(go.Scatter(x=x_qs, y=y_qs, mode='lines', name='Predicted Queue Size')) # calculate by this py
    # add queue size
    fig.add_trace(go.Scatter(x=x_qs_rtc, y=y_qs_rtc, mode='lines', name='Predicted Queue Size'))
    
    # add x_list & y_list
    for keyword in keywords:
        # print(x_list[keyword])
        # print(y_list[keyword])
        fig.add_trace(go.Scatter(x=x_list[keyword], y=y_list[keyword], mode='lines', name=names[keyword]))
    
    
    # y range 0-1000
    # fig.update_yaxes(range=[-200, 300])
    # 可以拖动调整y轴范围
    fig.update_yaxes(fixedrange=False)
    
    # rangeslider visible
    fig.update_layout(xaxis_rangeslider_visible=True)

    fig.update_layout(title='Packets',
                        xaxis_title='time',
                        yaxis_title='packets')

    fig.write_html( os.path.join(logpath, 'timeline.html'))
    
    # fig.write_html( 'view/timeline.html')
    
    # os.system("git commit -am 'update'")
    # os.system("git push")
    

    



    
            
            
            