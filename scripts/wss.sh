#!/bin/bash

MIN_NR_ACCESSES=$1

FILES="
  spec/433.milc-1.data
  spec/462.libquantum-1.data
  spec/470.lbm-1.data
  npb/cg.B-1.data
  npb/sp.B-1.data
  parsec/ferret-1.data
  splash2x/water_nsquared-1.data
  splash2x/fft-1.data
  splash2x/volrend-1.data"

for f in $FILES
do
	./wss.py -o ../../daptrace.data/$f
done

