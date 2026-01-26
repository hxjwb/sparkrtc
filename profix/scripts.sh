
dir=archive/proace_20251120_094722
python3 prfl.py $dir > $dir/a.log
python3 second.py --log $dir/a.log --outdir $dir
