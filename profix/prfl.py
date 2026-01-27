# import imp
import re
import numpy as np
time_sync = None
isLlama = False # False for webrtc, True for Llama

def check_type(packet_type):
#     enum class RtpPacketMediaType : size_t {
#   kAudio,                         // Audio media packets.
#   kVideo,                         // Video media packets.
#   kRetransmission,                // Retransmisions, sent as response to NACK.
#   kForwardErrorCorrection,        // FEC packets.
#   kPadding = kNumMediaTypes - 1,  // RTX or plain padding sent to maintain BWE.
#   // Again, don't forget to update `kNumMediaTypes` if you add another value!
# };
    if not isLlama:
        if packet_type == 1:
            return 'media'
        elif packet_type == 2:
            return 'rtx'
        elif packet_type == 3:
            return 'fec'
        elif packet_type == 4:
            return 'padding'
        elif packet_type == 5:
            return 'unknown'
    else:
        return 'unknown'
    
class Frame:
    def __init__(self, rtp_ts, f_size, encode_duration, time):
        # Metadata for the frame
        self.rtp_ts = rtp_ts
        self.f_size = f_size
        
        # Life-cycle timestamps
        self.captured_time = time - encode_duration
        self.encoded_time = time
        self.received = False
        self.assembled_time = None
        self.todecode_time = None
        self.decoded_time = None
        
        # packet
        # self.packet_seqs = []
        self.media_packets = []
        self.recv_packets = []
        
    
    def __repr__(self):
        return (f"Frame(rtp_ts={self.rtp_ts}, f_size={self.f_size}, "
                f"captured_time={self.captured_time}, encoded_time={self.encoded_time}, "
                f"received={self.received}, assembled_time={self.assembled_time}, "
                f"todecode_time={self.todecode_time}, decoded_time={self.decoded_time})")

    def net_delay(self):
        if time_sync is not None and self.assembled_time is not None and self.encoded_time is not None:
            # times in receiver side should minus time sync
            return self.assembled_time - time_sync - self.encoded_time
        return None
    
    def encode_delay(self):
        if self.encoded_time is not None and self.captured_time is not None:
            return self.encoded_time - self.captured_time
        return None
    
    def todecode_queue_delay(self):
        if self.todecode_time is not None and self.assembled_time is not None:
            return self.todecode_time - self.assembled_time
        return None
    
    def decode_delay(self):
        if self.decoded_time is not None and self.todecode_time is not None:
            return self.decoded_time - self.todecode_time
        return None

    def timegap(self):
        net_delay = self.net_delay()
        encode_delay = self.encode_delay()
        todecode_queue_delay = self.todecode_queue_delay()
        decode_delay = self.decode_delay()
        
        return (f"RTP TS: {self.rtp_ts}, Frame Size: {self.f_size}, Encode {encode_delay}, Net {net_delay}, Wait {todecode_queue_delay}, Decode {decode_delay} E2E {self.e2e_delay()} ")
    def e2e_delay(self):
        if time_sync is not None and self.decoded_time is not None and self.captured_time is not None:
            return self.decoded_time - time_sync - self.captured_time 
        return None
    
    
class Packet:
    def __init__(self, uid):
        self.uid = uid # Unique identifier for the packet
        
        self.rtp_ts = None # RTP timestamp packet belongs to
        
        self.type = None 
        self.size = None 
        self.payload_head = None
        
        self.send_time = None 
        self.recv_time = None
        self.seq_num = None # Sequence number of the packet
        self.original_seq = None # Original sequence number for retransmissions
        self.retran = []

    def add_retran(self, uid):
        self.retran.append(uid)

    def get_recv_time(self):
        if time_sync is not None and self.recv_time is not None:
            return self.recv_time - time_sync
        else:
            return None


def get_time(line):
    # [000:730][1958114] (rtp_sender.cc:480):
    # [second:millisecond][threadnum] (file:line):
    match = re.search(r'\[(\d+):(\d+)\]', line)
    if match:
        seconds = int(match.group(1))
        milliseconds = int(match.group(2))
        # print(f"Seconds: {seconds}, Milliseconds: {milliseconds}")
        return seconds * 1000 + milliseconds
    else:
        print("No time found in line.")
        return None

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description='Process log files.')
    parser.add_argument('file', type=str,  help='Path to the log file directory')
    args = parser.parse_args()
    
    file_dir = args.file
    send_log = f"{file_dir}/send_0"
    recv_log = f"{file_dir}/recv_0"
    
    lines_send = open(send_log, 'r').readlines()
    lines_recv = open(recv_log, 'r').readlines()
    
    rtp2frames = {}
    
    # uid2media_packets = {}
    # uid2rtx_packets = {}
    # uid2fec_packets = {}
    # uid2padding_packets = {}
    
    # seq2uid = {}

    uid2packets = {}
    seq_payload_to_uid = {}
    seq_to_uids = {}
    send_packet_count = 0
    missing_recv_rtp_ts = 0
    
    # Go through send log
    for line in lines_send:
        # Frame level
        if 'Prfl_frame_send' in line:
            time = get_time(line)
            line_parts = line.split('@')[1].split(' ')
            rtp_ts = int(line_parts[0])
            f_size = int(line_parts[1])
            encode_duration = int(line_parts[2])
            frame = Frame(rtp_ts, f_size, encode_duration, time)
            rtp2frames[rtp_ts] = frame
            

    for line in lines_send:
        # Packet level
        if 'Prfl_pkt_send' in line:
            # Prfl_pkt_send@2850912143 8690 1 5c4123b569751f50f645 1185 8690
            # print(line)
            time = get_time(line)
            line_parts = line.split('@')[1].split(' ')
            if len(line_parts) < 6:
                print(f"Line format error: {line}")
                continue
            rtp_ts = int(line_parts[0])
            seq_num = int(line_parts[1])
            packet_type = int(line_parts[2])
            packet_type = check_type(packet_type)
            payload_head = line_parts[3]
            # uid = payload_head.strip()[2:] + f'{seq_num}'  
            uid = f'{rtp_ts} {seq_num}'  
            size = int(line_parts[4])
            original_seq = int(line_parts[5]) 
            # print(f"Processing packet with UID: {uid}, RTP TS: {rtp_ts}, Seq Num: {seq_num}, Type: {packet_type}, Size: {size}, Original Seq: {original_seq}")
            
            # create packet
            packet = Packet(uid)
            packet.rtp_ts = rtp_ts
            packet.type = packet_type
            packet.size = size
            packet.payload_head = payload_head
            packet.send_time = time
            packet.original_seq = original_seq
            packet.seq_num = seq_num

            uid2packets[uid] = packet
            send_packet_count += 1
            seq_payload_to_uid[(seq_num, payload_head)] = uid
            seq_to_uids.setdefault(seq_num, []).append(uid)

            if packet_type == 'media':
                if rtp_ts not in rtp2frames:
                    print(f"Frame with RTP timestamp {rtp_ts} not found in send log.")
                    continue
                frame = rtp2frames[rtp_ts]
                frame.media_packets.append(uid)
                    
            if packet_type == 'rtx':
                if rtp_ts not in rtp2frames:
                    print(f"Frame with RTP timestamp {rtp_ts} not found in send log.")
                    continue
                # find the corresponding media packet
                media_packet = None
                for mp_uid in rtp2frames[rtp_ts].media_packets:
                    if uid2packets[mp_uid].original_seq == original_seq:
                        media_packet = uid2packets[mp_uid]
                        break
                if media_packet is None:
                    print(f"Media packet with seq num {original_seq} not found in frame with RTP timestamp {rtp_ts}.")
                    continue
                media_packet.add_retran(uid)

                
        
            
            
            
    # Go through recv log
    for line in lines_recv:
        # Frame level
        if 'Prfl_frame_recv' in line:
            time = get_time(line)
            line_parts = line.split('@')[1].split(' ')
            rtp_ts = int(line_parts[0])
            decode_duration = int(line_parts[1])
            if rtp_ts in rtp2frames:
                frame = rtp2frames[rtp_ts]
                frame.received = True
                frame.decoded_time = time
                frame.todecode_time = time - decode_duration
            else:
                print(f"Frame with RTP timestamp {rtp_ts} not found in send log.")
        if 'Prfl_frame_assemble' in line:
            time = get_time(line)
            line_parts = line.split('@')[1].split(' ')
            rtp_ts = int(line_parts[0])
            if rtp_ts in rtp2frames:
                frame = rtp2frames[rtp_ts]
                frame.assembled_time = time
            else:
                print(f"Frame with RTP timestamp {rtp_ts} not found in send log.")
                
    seq_to_rtp_ts = {}
    seq_to_size = {}
    for line in lines_recv:
        # Packet level
        if 'Prfl_pkt_re2v' in line:
            # Prfl_pkt_re2v@5c41014d525c71cb627a 20888 1311020365 4087395144
            
            time = get_time(line)
            line_parts = line.split('@')[1].split(' ')
            if len(line_parts) < 4:
                print(f"Line format error: {line}")
                continue
            payload_head = line_parts[0].strip()
            seq_num = int(line_parts[1])
            rtp_ts = int(line_parts[2])
            ssrc = int(line_parts[3])
            # uid = payload_head[:-2] + f'{seq_num}'
            uid = f'{rtp_ts} {seq_num}'
            # print(f"Processing packet with UID: {uid}")
            # print(uid2media_packets)
            # print(uid2media_packets[uid])
            if uid in uid2packets:
                packet = uid2packets[uid]
                packet.recv_time = time
        if 'Received RTP packet with SSRC' in line:
            time = get_time(line)
            match = re.search(
                r'seq num:\s+(\d+),.*payload size:\s+(\d+), payload:\s*([0-9a-fA-F]*)',
                line)
            if match:
                seq_num = int(match.group(1))
                payload_size = int(match.group(2))
                payload_hex = match.group(3).strip()
                seq_to_size[seq_num] = payload_size
                if payload_hex:
                    payload_head = payload_hex[:20]
                    match_uid = seq_payload_to_uid.get((seq_num, payload_head))
                    if match_uid is None:
                        candidates = seq_to_uids.get(seq_num, [])
                        if len(candidates) == 1:
                            match_uid = candidates[0]
                    if match_uid is not None:
                        packet = uid2packets[match_uid]
                        packet.recv_time = time
                        rtp_ts = packet.rtp_ts
                        if rtp_ts in rtp2frames:
                            frame = rtp2frames[rtp_ts]
                            if match_uid not in frame.recv_packets:
                                frame.recv_packets.append(match_uid)
        if 'Packet received on SSRC' in line:
            match = re.search(r'timestamp:\s+(\d+), sequence number:\s+(\d+)', line)
            if match:
                rtp_ts = int(match.group(1))
                seq_num = int(match.group(2))
                seq_to_rtp_ts[seq_num] = rtp_ts
        if 'Prfl_pkt_recv' in line:
            # Prfl_pkt_recv@78001867640028acb403 18484
            time = get_time(line)
            line_parts = line.split('@')[1].split(' ')
            if len(line_parts) < 2:
                print(f"Line format error: {line}")
                continue
            payload_head = line_parts[0].strip()
            seq_num = int(line_parts[1])
            rtp_ts = seq_to_rtp_ts.get(seq_num)
            if rtp_ts is None:
                match_uid = seq_payload_to_uid.get((seq_num, payload_head))
                if match_uid is None:
                    candidates = seq_to_uids.get(seq_num, [])
                    if len(candidates) == 1:
                        match_uid = candidates[0]
                if match_uid is None:
                    missing_recv_rtp_ts += 1
                    continue
                packet = uid2packets[match_uid]
                rtp_ts = packet.rtp_ts
                if rtp_ts in rtp2frames:
                    frame = rtp2frames[rtp_ts]
                    if match_uid not in frame.recv_packets:
                        frame.recv_packets.append(match_uid)
                continue
            uid = f'{rtp_ts} {seq_num}'
            if uid in uid2packets:
                packet = uid2packets[uid]
            else:
                packet = Packet(uid)
                packet.rtp_ts = rtp_ts
                packet.type = 'unknown'
                packet.size = None
                packet.payload_head = payload_head
                packet.seq_num = seq_num
                uid2packets[uid] = packet
            if packet.size is None and seq_num in seq_to_size:
                packet.size = seq_to_size[seq_num]
            if rtp_ts in rtp2frames:
                frame = rtp2frames[rtp_ts]
                if uid not in frame.recv_packets:
                    frame.recv_packets.append(uid)
    
    # Get time sync by minimum delay of media packets
    packet_delays = []
    for uid, packet in uid2packets.items():
        if packet.recv_time is not None and packet.send_time is not None:
            delay = packet.recv_time - packet.send_time
            packet_delays.append(delay)

    if packet_delays:
        time_sync = min(packet_delays)
        print(f"Time sync: {time_sync} ms")
    else:
        frame_delays = []
        for rtp_ts, frame in rtp2frames.items():
            if frame.assembled_time is not None and frame.encoded_time is not None:
                frame_delays.append(frame.assembled_time - frame.encoded_time)
            elif frame.decoded_time is not None and frame.encoded_time is not None:
                frame_delays.append(frame.decoded_time - frame.encoded_time)
        if frame_delays:
            time_sync = min(frame_delays)
            print(f"Time sync (fallback from frames): {time_sync} ms")
        else:
            time_sync = 0
            print("Time sync: 0 ms (fallback)")
    
                   
    print("-" * 50)
    
    # find tail frames
    

    # # Print all frames
    for rtp_ts, frame in rtp2frames.items():
        print(frame.timegap())
        packets = list(frame.media_packets)
        if frame.recv_packets:
            existing = set(packets)
            for uid in frame.recv_packets:
                if uid not in existing:
                    packets.append(uid)
        # retran  = False
        lossed_packets = 0
        all_packets = len(packets)
        for uid in packets:
            # if there is retransmission
            packet = uid2packets[uid]
            if packet.send_time is not None:
                send_delta = packet.send_time - frame.encoded_time
            else:
                send_delta = None
            if packet.send_time is not None:
                if packet.get_recv_time() is None:
                    trans_delta = '"lost"'
                else:
                    trans_delta = packet.get_recv_time() - packet.send_time
            else:
                trans_delta = None
            if packet.get_recv_time() is not None and frame.encoded_time is not None:
                recv_delta = packet.get_recv_time() - frame.encoded_time
            else:
                recv_delta = None
            print(f"P: seq {packet.seq_num}, size {packet.size}, send_delta {send_delta}, trans_delta {trans_delta}, recv_delta {recv_delta}")
            if packet.retran != []:
                # print retransmission packet
                for retran_uid in packet.retran:
                    retran_packet = uid2packets[retran_uid]
                    if retran_packet.get_recv_time() is None:
                        trans_delta = None
                    else:
                        trans_delta = retran_packet.get_recv_time() - retran_packet.send_time
                    print(f"-- R: seq {retran_packet.seq_num}, size {retran_packet.size}, send_delta {retran_packet.send_time - frame.encoded_time}, trans_delta {trans_delta}")
                # lossed_packets += 1
           