



def change(symbol, value, file):
    lines = open(file, 'r').readlines()
    new_line = f'{symbol} = {value};'

    new_lines = []
    for line in lines:
        if symbol in line:
            print(line)
            print(new_line)
            new_lines.append(new_line + '\n')
        else:
            new_lines.append(line)

    open(file, 'w').writelines(new_lines)

delay = 5
loss = 0.5
loss_start = 30000
loss_duration = 500



    
cc_file = '/home/xiangjie/mahimahi/src/frontend/link_queue.cc'

change('uint64_t FIXED_DELAY_MS', delay, cc_file)
change('uint64_t FIXED_LOSS_START', loss_start, cc_file)
change('uint64_t FIXED_LOSS_DURATION', loss_duration, cc_file)



hh_file = '/home/xiangjie/mahimahi/src/frontend/link_queue.hh'
change('    double loss_ratio_', loss, hh_file)

# sudo make install
import os
os.system('cd /home/xiangjie/mahimahi && sudo make install')
