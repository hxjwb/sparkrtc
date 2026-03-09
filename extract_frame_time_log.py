import re

input_file = '/Users/bytedance/sparkrtc/logs/send_0'
output_file = '/Users/bytedance/sparkrtc/logs/frame_time_window.log'

pattern = re.compile(r'\(\s*frame_time_window\.cc:\s*\d+\s*\):\s*(.*)$')

extracted_lines = []

with open(input_file, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
        if 'frame_time_window.cc' in line:
            match = pattern.search(line)
            if match:
                extracted_lines.append(match.group(1))
            else:
                parts = line.split('(frame_time_window.cc:')
                if len(parts) > 1:
                    part = parts[1].split('):', 1)
                    if len(part) > 1:
                        extracted_lines.append(part[1].strip())

with open(output_file, 'w', encoding='utf-8') as f:
    for line in extracted_lines:
        f.write(line + '\n')

print(f'Extracted {len(extracted_lines)} lines to {output_file}')
