#!/usr/bin/env bash

TOTAL_PHYSICAL_CORES=$(grep '^core id' /proc/cpuinfo | sort -u | wc -l)
TOTAL_LOGICAL_CORES=$(grep '^core id' /proc/cpuinfo | wc -l)

# Load MSR module
sudo modprobe msr

# Setup
samples=60000 # 1 minute @ 1 second == 1000
outer=1
num_thread=$TOTAL_LOGICAL_CORES
date=$(date +"%m%d-%H%M")

### Warm Up ###
stress-ng -q --cpu $TOTAL_LOGICAL_CORES -t 10m

rm -rf out
mkdir out
rm -rf input.txt

for selector in $(seq 0 16); do
  echo $selector >>input.txt
done

### Simulate w/ dynamic instruction throttling
sudo ./bin/driver ${num_thread} ${samples} ${outer} 80
cp -r out data/out-${date}

### Simulate w/o dynamic instruction throttling
sudo ./bin/driver ${num_thread} ${samples} ${outer} 2000
cp -r out data/out-${date}

# Unload MSR module
sudo modprobe -r msr
