#!/usr/bin/env python

"Print out distribution of working set sizes of given trace"

import argparse
import struct
import sys

percentiles = [0, 25, 50, 75, 100]

parser = argparse.ArgumentParser()
parser.add_argument('tracefile', type=str, metavar='<trace file>',
        help='path to trace output file (bin format)')
parser.add_argument('--range', '-r', type=int, nargs=3,
        metavar=('<start>', '<stop>', '<step>'),
        help='range of wss percentiles to print')
parser.add_argument('--sortby', '-s', choices=['time', 'size'],
        help='metric to be used for sorting working set sizes')
parser.add_argument('--oneline', '-o', default=False, action='store_true',
        help='print output in one line')

args = parser.parse_args()
file_path = args.tracefile
if args.range:
    percentiles = range(args.range[0], args.range[1], args.range[2])
wss_sort = True
if args.sortby == 'time':
    wss_sort = False
oneline_output = args.oneline

def patterns(f):
    wss = 0
    nr_regions = struct.unpack('I', f.read(4))[0]

    patterns = []
    for r in range(nr_regions):
        saddr = struct.unpack('L', f.read(8))[0]
        eaddr = struct.unpack('L', f.read(8))[0]
        nr_accesses = struct.unpack('I', f.read(4))[0]
        patterns.append([eaddr - saddr, nr_accesses])
    return patterns

pid_pattern_map = {}
with open(file_path, 'rb') as f:
    start_time = None
    while True:
        timebin = f.read(16)
        if len(timebin) != 16:
            break
        nr_tasks = struct.unpack('I', f.read(4))[0]
        for t in range(nr_tasks):
            pid = struct.unpack('L', f.read(8))[0]
            if not pid_pattern_map:
                pid_pattern_map[pid] = []
            pid_pattern_map[pid].append(patterns(f))

for pid in pid_pattern_map.keys():
    snapshots = pid_pattern_map[pid][20:]
    nr_regions_dist = []
    for snapshot in snapshots:
        nr_regions_dist.append(len(snapshot))
    if wss_sort:
        nr_regions_dist.sort(reverse=False)

    to_print = "%s: " % pid
    for percentile in percentiles:
        thres_idx = int(percentile / 100.0 * len(nr_regions_dist))
        if thres_idx == len(nr_regions_dist):
            thres_idx -= 1
        threshold = nr_regions_dist[thres_idx]
        if oneline_output:
            to_print += "%d,\t" % (nr_regions_dist[thres_idx])
        else:
            to_print += "\n%d" % (nr_regions_dist[thres_idx])
    if oneline_output:
        to_print += "%d" % (sum(nr_regions_dist) / len(nr_regions_dist))
    else:
        to_print += "\n%d" % (sum(nr_regions_dist) / len(nr_regions_dist))
    print to_print
