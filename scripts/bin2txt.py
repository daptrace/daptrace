#!/usr/bin/env python

import struct
import sys

if len(sys.argv) != 2:
    print "Usage: %s <bin file path>" % sys.argv[0]
    sys.exit(1)

file_path = sys.argv[1]

def parse_time(bindat):
    "bindat should be 16 bytes"
    sec = struct.unpack('l', bindat[0:8])[0]
    nsec = struct.unpack('l', bindat[8:16])[0]
    return sec * 1000000000 + nsec;

def pr_region(f):
    saddr = struct.unpack('L', f.read(8))[0]
    eaddr = struct.unpack('L', f.read(8))[0]
    nr_accesses = struct.unpack('I', f.read(4))[0]
    print "%012x-%012x(%10d):\t%d" % (saddr, eaddr, eaddr - saddr, nr_accesses)


def pr_task_info(f):
    pid = struct.unpack('L', f.read(8))[0]
    print "pid: ", pid
    nr_regions = struct.unpack('I', f.read(4))[0]
    print "nr_regions: ", nr_regions
    for r in range(nr_regions):
        pr_region(f)

with open(file_path, 'rb') as f:
    start_time = None
    while True:
        timebin = f.read(16)
        if len(timebin) != 16:
            break
        time = parse_time(timebin)
        if not start_time:
            start_time = time
            print "start_time: ", start_time
        print "rel time: %16d" % (time - start_time)
        nr_tasks = struct.unpack('I', f.read(4))[0]
        print "nr_tasks: ", nr_tasks
        for t in range(nr_tasks):
            pr_task_info(f)
            print ""
