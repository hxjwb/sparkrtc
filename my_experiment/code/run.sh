#!/bin/bash
width=1920
height=1080

usage() {
	echo "[Usage] $0 [-i <video_name>] [-p <program_name>]" 1>&2;
	# echo "[Usage] The optional programs contain: all(run all following programs), "
	# echo "[Usage]                                gen_send_video, send_and_recv"
	# echo "[Usage]                                decode_recv_video and show_fig"
	# echo "[Usage] The default size is 1920x1080, can modified by [-s <{width}x{height}]"
	# echo "[Usage] For example: $0 -i video_0a86 -p all -o test_output -s 1920x1080"
	exit 1;
}

while getopts ":i:p:s:" opt; do
    case "${opt}" in
        i)
            video_name=${OPTARG}
            ;;
        p)
            p=${OPTARG}
			if [ $p == "all" ] || [ $p == "send_and_recv" ] || [ $p == "gen_send_video" ] ||
			   [ $p == "decode_recv_video" ] || [ $p == "show_fig" ]; then
				run_program=${p}
			else
				usage
			fi
            ;;
		s)
			s=${OPTARG}
			s=(${s//x/ })
			width=${s[0]}
			height=${s[1]}
			;;
        *)
            usage
            ;;
    esac
done
shift $((OPTIND-1))

if [ -z "${video_name}" ] || [ -z "${run_program}" ]; then
    usage
fi

trace_logs_dir="../file/trace_logs"

if [ $run_program == "gen_send_video" ]; then
	python3 process_video_qrcode.py --option=gen_send_video --data=$video_name --height=$height --width=$width
	exit 0
fi

# minQP=(2 10 15 20 26)
# maxQP=(51 45 40 35 35)

vbv_ratios=(0.03 0.04 0.06 0.08 0.1 0.2 0.5 1.0)
# vbv_ratios=(0.03 0.1 0.3 0.5 0.7 1.0 2.0 5.0 7.0 10.0 15.0)
# vbv_ratios=(0.1 0.5 1.0 5.0 7.0 10.0 15.0 30.0)
# vbv_ratios=(0.3 0.4 0.5 0.6 0.7)
# vbv_ratios=(0.03 10.0 15.0)
vbv_ratios=(0.04)
# vbv_ratios=(0.03 0.1 0.2 0.3 0.4 0.5 0.7 1.0 10.0 15.0)
# encoder_coefficients=(-0.5 -0.2 -0.1 -0.06 0 0.03 0.06 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1.0 2.0)
encoder_coefficients=(0.9 1.0 2.0)
low_bitrate_vbv=(0.7 0.8 1.0)
low_bitrate_vbv=(0.7)
minQP=(10)
maxQP=(45)

length=${#low_bitrate_vbv[@]}

SendAndDecode() {
	# parameter list: 1.output_dir 2.vbv_ratio 3.video_name 4.minQP 5.maxQP 6.height 7.width 8.focusDropPeriod 9.low_bitrate_vbv
	python3 process_video_qrcode.py --option=send_and_recv --data=$3 --minQP=$4\
	--maxQP=$5 --vbvRatio=$2 --focusDropPeriod=$8\
	--height=$6 --width=$7  --output_dir=$1 --encoderAddCoefficient=$9
	converged=$?
	pid=$!
	wait $pid
	find ../result -name 'recon.yuv' -delete
	return $converged
}

for file in $(find ${trace_logs_dir} -maxdepth 1 -type f)
do
	for vbv_ratio in ${vbv_ratios[@]}
	do
		for ((i=0; i<length; i++))
		do
			filename=$(basename -- "$file")
			filename="${filename%.*}"
			# filename="static_500kbps"
			# filename="static_1mbps"
			filename="10s_10to1_until_300s"
			# filename="wifi-223sid_251_5"
			# vbv_ratio=0.65
			focus_drop_period=0

			converged=0
			times=0

			if [ $run_program == "all" ] || [ $run_program == "send_and_recv" ]; then
				# general send process
				while [ $converged == 0 ]
				do
					output_dir="${filename}/x264_${vbv_ratio}_${times}"
					# output_dir="${filename}/x264_adaptive_${times}"
					# output_dir="${filename}/vpx_${times}"
					echo $output_dir
					SendAndDecode ${output_dir} ${vbv_ratio} ${video_name} ${minQP[0]} ${maxQP[0]} ${height} ${width} ${focus_drop_period} ${low_bitrate_vbv[i]}
					converged=$?
					times=$((times+1))
					if [ $times -gt 10 ]; then
						break
					fi
					echo "The return value is: $converged"
					exit 0
				done
				if [ -e "../last_average_record.log" ]; then
					rm "../last_average_record.log"
				fi
				if [ -e "../last_average_record_drop_period.log" ]; then
					rm "../last_average_record_drop_period.log"
				fi
			fi
			# send_and_recv contains decode process
			if [ $run_program == "decode_recv_video" ]; then
				output_dir="${filename}/x264_${vbv_ratio}_0"
				python3 process_video_qrcode.py --option=decode_recv_video --data=$video_name --height=$height --width=$width  --output_dir=${output_dir}
				pid=$!
				wait $pid
			fi
			if [ $run_program == "all" ] || [ $run_program == "show_fig" ]; then
				output_dir="${filename}/x264_${vbv_ratio}_0"
				python3 process_video_qrcode.py --option=show_fig --data=$video_name  --output_dir=${output_dir}
			fi
		done # qp range
		exit 0
	done # vbv_ratios
	exit 0
done # trace logs
