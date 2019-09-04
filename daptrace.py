#!/usr/bin/env python

"""
Trace data access patterns of given command.

TODOs
- 'poll()' instead of 'wait()' for the command and add childs to the pids if
  created
"""


import argparse
import copy
import os
import signal
import subprocess
import sys
import atexit

DBGFS="/sys/kernel/debug/mapia/"
DBGFS_ATTRS = DBGFS + "attrs"
DBGFS_PIDS = DBGFS + "pids"
DBGFS_TRACING_ON = DBGFS + "tracing_on"

def dprint(msg):
    print "[daptrace] " + msg

def sighandler(signum, frame):
    if signum == signal.SIGINT:
        subprocess.call("echo off > %s" % DBGFS_TRACING_ON,
                shell=True, executable="/bin/bash")
    else:
        print signum

def chk_prerequisites():
    if os.geteuid() != 0:
        print "Run as root"
        exit(1)

    atexit.register(exit_handler)

    if not os.path.isdir(DBGFS):
        kmod_path = os.path.dirname(sys.argv[0]) + "/kernel"
        dprint("Buildling daptrace kernel module..")
        os.system("cd %s && make" % kmod_path)
        dprint("Inserting daptrace kernel module..")
        os.system("cd %s && insmod daptrace.ko" % kmod_path)

    if not os.path.isfile(DBGFS_PIDS):
        dprint("mapia pids file (%s) not exists." % DBGFS_PIDS)
        exit(1)

class Attrs:
    sample_interval = None
    aggr_interval = None
    regions_update_interval = None
    min_nr_regions = None
    max_nr_regions = None
    rfile_path = None

    def __init__(self, s, a, r, n, x, f):
        self.sample_interval = s
        self.aggr_interval = a
        self.regions_update_interval = r
        self.min_nr_regions = n
        self.max_nr_regions = x
        self.rfile_path = f

    def __str__(self):
        return "%s %s %s %s %s %s" % (self.sample_interval, self.aggr_interval,
                self.regions_update_interval, self.min_nr_regions,
                self.max_nr_regions, self.rfile_path)

def read_attrs():
    with open(DBGFS_ATTRS, 'r') as f:
        attrs = f.read().split()
    atnrs = [int(x) for x in attrs[0:5]]
    attrs = atnrs + [attrs[5]]
    return Attrs(*attrs)

def apply_attrs_args(args):
    a = read_attrs()
    if args.sample:
        a.sample_interval = args.sample
    if args.aggr:
        a.aggr_interval = args.aggr
    if args.updr:
        a.regions_update_interval = args.updr
    if args.minr:
        a.min_nr_regions = args.minr
    if args.maxr:
        a.max_nr_regions = args.maxr
    if args.out:
        a.rfile_path = args.out

    subprocess.call("echo %s > %s" % (a, DBGFS_ATTRS), shell=True,
            executable="/bin/bash")

    return args.command

def handle_args():
    "Define, parse, apply options and return the main command"
    parser = argparse.ArgumentParser()
    parser.add_argument('command', type=str, help='the command to trace')
    parser.add_argument('-s', '--sample', type=int, help='sampling interval')
    parser.add_argument('-a', '--aggr', type=int, help='aggregate interval')
    parser.add_argument('-u', '--updr', type=int,
            help='regions update interval')
    parser.add_argument('-n', '--minr', type=int,
            help='minimal number of regions')
    parser.add_argument('-m', '--maxr', type=int,
            help='maximum number of regions')
    parser.add_argument('-o', '--out', type=str, help='output file path')

    args = parser.parse_args()

    return args

def exit_handler():
    os.system("rmmod daptrace")

if __name__ == '__main__':
    args = handle_args()

    chk_prerequisites()

    signal.signal(signal.SIGINT, sighandler)
    orig_attrs = read_attrs()
    cmd = apply_attrs_args(args)

    a = read_attrs()
    if os.path.isfile(a.rfile_path):
        dprint("Move %s to %s.old" % (a.rfile_path, a.rfile_path))
        os.rename(a.rfile_path, a.rfile_path + ".old")

    dprint("Start tracing...")
    p = subprocess.Popen(cmd, shell=True, executable="/bin/bash")
    subprocess.call("echo %s > %s" % (p.pid, DBGFS_PIDS),
            shell=True, executable="/bin/bash")
    subprocess.call("echo on > %s" % DBGFS_TRACING_ON,
            shell=True, executable="/bin/bash")
    p.wait()
    subprocess.call("echo off > %s" % DBGFS_TRACING_ON,
            shell=True, executable="/bin/bash")

    subprocess.call("echo %s > %s" % (orig_attrs, DBGFS_ATTRS), shell=True,
            executable="/bin/bash")
    dprint("Tracing done.")
