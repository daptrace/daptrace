#!/usr/bin/env python

import os
import shutil
import argparse
from struct import unpack
from struct import pack

def group_by_pid(binfile):
    pids = []
    with open(binfile, 'rb') as encoded:
        stop_parsing = 0
        filegroup = {}
        while encoded.read(1):
            encoded.seek(-1, 1)
            sec = unpack('l', encoded.read(8))[0]
            nsec = unpack('l', encoded.read(8))[0]

            nr_tasks = unpack('I', encoded.read(4))[0]
            if len(encoded.read(12)) != 12:
                break
            encoded.seek(-12, 1)

            for task in range(nr_tasks):
                pid = unpack('L', encoded.read(8))[0]
                nr_regions = unpack('I', encoded.read(4))[0]
                if len(encoded.read(nr_regions * 20)) != nr_regions * 20:
                    stop_parsing = 1
                    break
                encoded.seek(-20 * nr_regions, 1)

                if pid not in pids:
                    pids.append(pid)
                    pidfile = '.' + str(pid) + '.data'
                    filegroup[pid] = open(pidfile, 'wb')

                outfile = filegroup[pid]

                # Write time and basic info
                outfile.write(pack('l', sec))
                outfile.write(pack('l', nsec))
                outfile.write(pack('I', 1))         # nr_tasks = 1
                outfile.write(pack('L', pid))
                outfile.write(pack('I', nr_regions))

                # Write a whole bunch of regions
                outfile.write(encoded.read(nr_regions * 20))

            if stop_parsing:
                break

        for pid in pids:
            filegroup[pid].close()

    return pids


def rm_grouped_files(binfile, pids):
    for pid in pids:
        pidfile = '.' + str(pid) + '.data'
        os.remove(pidfile)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--data', type = str, default = 'nodata',
            help = 'input data file')

    args = parser.parse_args()
    if args.data == 'nodata':
        print "No data provided"
    else:
        pids = group_by_pid(args.data)
        print pids


if __name__ == "__main__":
    main()
