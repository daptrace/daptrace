#!/usr/bin/env python
from __future__ import division
import re
import argparse
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
import matplotlib.ticker as ticker
from warnings import simplefilter
from matplotlib.ticker import EngFormatter
from collections import OrderedDict
from struct import unpack
from groupinput import group_by_pid, rm_grouped_files

S_IN_NS = 1000000000

def page_align(addr):
    page_shift = 12
    page_size = 1 << page_shift
    page_mask = (~(page_size - 1))

    return (addr + page_size - 1) & page_mask


def next_page(addr):
    page_shift = 12
    page_size = 1 << page_shift

    return addr + page_size


def step_align_floor(addr, step_size):
    return (addr // step_size) * step_size


def get_time_info(binfile):
    basetime = None
    endtime = None
    with open(binfile, 'rb') as encoded:
        stop_parsing = 0
        starttime = None
        while encoded.read(1):
            encoded.seek(-1, 1)
            sec = unpack('l', encoded.read(8))[0]
            nsec = unpack('l', encoded.read(8))[0]
            time = sec * S_IN_NS + nsec

            if not basetime and starttime:
                basetime = starttime - (time - starttime)
            if not starttime:
                starttime = time

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

            if stop_parsing:
                break

            endtime = time

    return [basetime, endtime]


def extract_base_info(binfile, nr_columns):
    print "extracting base info.."
    max_nr_regions = 0
    nr_timestamps = 1
    range_used = {}
    [basetime, endtime] = get_time_info(binfile)
    max_nr_tps = 0
    stepsz = (endtime - basetime) // nr_columns

    # Time unit is nanosecond
    with open(binfile, 'rb') as encoded:
        steptime = -stepsz
        stop_parsing = 0
        nr_accu_regions = 0
        nr_time_per_step = 0
        while encoded.read(1):
            encoded.seek(-1, 1)
            sec = unpack('l', encoded.read(8))[0]
            nsec = unpack('l', encoded.read(8))[0]
            time = sec * S_IN_NS + nsec - basetime

            if steptime + stepsz < time:
                steptime = step_align_floor(time, stepsz)
                if nr_accu_regions > max_nr_regions:
                    max_nr_regions = nr_accu_regions
                nr_accu_regions = -1
                nr_timestamps = nr_timestamps + 1
                boundaries = {}
                if nr_time_per_step > max_nr_tps:
                    max_nr_tps = nr_time_per_step
                nr_time_per_step = 0

            nr_time_per_step = nr_time_per_step + 1

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
                for region in range(nr_regions):
                    start = unpack('L', encoded.read(8))[0]
                    end = unpack('L', encoded.read(8))[0]
                    nr_access = unpack('I', encoded.read(4))[0]

                    if start not in boundaries:
                        boundaries[start] = 1
                        nr_accu_regions = nr_accu_regions + 1
                    if end not in boundaries:
                        boundaries[end] = 1
                        nr_accu_regions = nr_accu_regions + 1

                    if start not in range_used:
                        range_used[start] = end
                    elif range_used[start] < end:
                        range_used[start] = end

            if stop_parsing:
                if nr_accu_regions > max_nr_regions:
                    max_nr_regions = nr_accu_regions
                break

    range_used = OrderedDict(sorted(range_used.items()))

    # Deal with overlapped ranges
    ranges = []
    new_range = (0, 0)
    for (start, end) in range_used.iteritems():
        if new_range[1] >= start:
            new_range = (new_range[0], max(new_range[1], end))
        else:
            ranges.append(new_range)
            new_range = (start, end)

    ranges.append(new_range)
    ranges = ranges[1:]

    return [max_nr_regions, nr_timestamps, ranges, basetime, max_nr_tps,\
            stepsz]


# Need optimization
def read_trace_data(binfile, nr_columns):
    baseinfo = extract_base_info(binfile, nr_columns)
    max_nr_regions = baseinfo[0]
    nr_timestamps = baseinfo[1]
    ranges = baseinfo[2]
    basetime = baseinfo[3]
    max_nr_time_per_step = baseinfo[4]
    stepsz = baseinfo[5]        # time step size in milliseconds

    print "generating plot data.."

    accmap = np.zeros([max_nr_regions, 2 * nr_timestamps - 3], dtype = float)
    pagemap = np.empty([max_nr_regions + 1, 2 * nr_timestamps - 2], dtype = int)
    timemap = np.zeros([max_nr_regions + 1, 2 * nr_timestamps - 2], dtype = float)

    time = 0
    with open(binfile, 'rb') as encoded:
        # Temporal variables
        stop_parsing = 0
        tidx = -2
        pgidx = 0
        addr = 0
        steptime = -stepsz
        local_data = np.empty([max_nr_regions * max_nr_time_per_step, 3])
        boundaries = {}

        # Start parsing
        while encoded.read(1):
            encoded.seek(-1, 1)
            sec = unpack('l', encoded.read(8))[0]
            nsec = unpack('l', encoded.read(8))[0]
            time = (sec * S_IN_NS + nsec) - basetime

            # Create a column in each timestep
            if steptime + stepsz < time:
                # Ignore cold hit
                if steptime >= 0:
                    # Sort boundaries in ascending order
                    boundaries = OrderedDict(sorted(boundaries.items()))

                    # Set x, y axis values
                    for (pgidx, addr) in enumerate(boundaries):
                        pagemap[pgidx, tidx : tidx + 2] = [addr, addr]
                        timemap[pgidx, tidx : tidx + 2] = \
                                [steptime / S_IN_NS, (steptime + stepsz) / S_IN_NS]
                        boundaries[addr] = pgidx

                    pgidx = pgidx + 1
                    if pgidx < max_nr_regions + 1:
                        timemap[pgidx : max_nr_regions + 1, tidx : tidx + 2] = \
                                [[steptime / S_IN_NS, (steptime + stepsz) / S_IN_NS]] * (max_nr_regions + 1 - pgidx)
                        pagemap[pgidx : max_nr_regions + 1, tidx : tidx + 2] = \
                                [[addr] * 2] * (max_nr_regions + 1 - pgidx)

                    # Write nr_access of each regions to accmap
                    idx = 0
                    while idx < lidx:
                        (start, end, nr_access) = local_data[idx]
                        sidx = boundaries[start]
                        eidx = boundaries[end]
                        aidx = sidx
                        while aidx < eidx:
                            accmap[aidx, tidx] = accmap[aidx, tidx] + \
                                (nr_access / (stepsz / S_IN_NS))
                            aidx = aidx + 1
                        idx = idx + 1

                # Prepare for the next timestep
                steptime = step_align_floor(time, stepsz)
                tidx = tidx + 2
                boundaries = {}
                lidx = 0
                nr_times = 0

            # Read number of tasks
            nr_tasks = unpack('I', encoded.read(4))[0]
            if len(encoded.read(12)) != 12:
                break
            encoded.seek(-12, 1)

            for task in range(nr_tasks):
                end_page = 0
                pid = unpack('L', encoded.read(8))[0]

                # Read number of regions in current task
                nr_regions = unpack('I', encoded.read(4))[0]
                if len(encoded.read(nr_regions * 20)) != nr_regions * 20:
                    stop_parsing = 1
                    break
                encoded.seek(-20 * nr_regions, 1)

                # Read data for each regions
                for region in range(nr_regions):
                    start = unpack('L', encoded.read(8))[0]
                    end = unpack('L', encoded.read(8))[0]
                    nr_access = unpack('I', encoded.read(4))[0]

                    start_page = page_align(start)
                    end_page = page_align(end)

                    if start_page not in boundaries:
                        boundaries[start_page] = 1
                    if end_page not in boundaries:
                        boundaries[end_page] = 1

                    # Save data for accumulation
                    local_data[lidx] = [start_page, end_page, nr_access]
                    lidx = lidx + 1

            if stop_parsing:
                break

            nr_times = nr_times + 1

        # Deal with remaining data
        boundaries = OrderedDict(sorted(boundaries.items()))

        # Set x, y axis values
        for (pgidx, addr) in enumerate(boundaries):
            pagemap[pgidx, tidx : tidx + 2] = [addr, addr]
            timemap[pgidx, tidx : tidx + 2] = \
                    [steptime / S_IN_NS, (steptime + stepsz) / S_IN_NS]
            boundaries[addr] = pgidx

        pgidx = pgidx + 1
        if pgidx < max_nr_regions + 1:
            timemap[pgidx : max_nr_regions + 1, tidx : tidx + 2] = \
                    [[steptime / S_IN_NS, (steptime + stepsz) / S_IN_NS]] * (max_nr_regions + 1 - pgidx)
            pagemap[pgidx : max_nr_regions + 1, tidx : tidx + 2] = \
                    [[addr] * 2] * (max_nr_regions + 1 - pgidx)

        # Write nr_access of each regions to accmap
        idx = 0
        while idx < lidx:
            (start, end, nr_access) = local_data[idx]
            sidx = boundaries[start]
            eidx = boundaries[end]
            aidx = sidx
            while aidx < eidx:
                accmap[aidx, tidx] = accmap[aidx, tidx] + (nr_access / nr_times)
                aidx = aidx + 1
            idx = idx + 1

    return [accmap, pagemap, timemap, ranges]


def plot(data, name, use_plain_address, nr_sig_digits, use_hex_address, vmax,
        adjust_bottom, show_ytitle, interactive_mode):
    print("drawing plot..")

    [accmap, pagemap, timemap, ranges] = data
    figname = name + '.pdf'

    nr_ranges = len(ranges)
    height_ratios = np.empty(nr_ranges)
    total_size = 0
    for i, (start, end) in enumerate(ranges):
        height_ratios[nr_ranges - 1 - i] = end - start
        total_size = total_size + (end - start)

    title = name + " (total: " + str(total_size // (1024 * 1024)) + "MiB)"

    max_height = 0
    for height in height_ratios:
        if height > max_height:
            max_height = height

    tick_interval = max_height / 5
    ticks = [[]] * nr_ranges
    for i, (start, end) in enumerate(ranges):
        if i != 2:
            ticks[i] = np.arange(start, end + 1, tick_interval)
        if i == 0 and adjust_bottom:
            ticks[i] = ticks[i][0:-1]

    # Formatter for Kilo, Mega, Giga, and so on.
    metric_prefix_formatter = EngFormatter(places = nr_sig_digits)

    # Set format of font
    plt.rcParams['font.family'] = 'serif'
    plt.rcParams['font.serif'] = ['Times New Roman'] + plt.rcParams['font.serif']

    # Create subplots with height ratios using gridspec
    fig, ax = plt.subplots(nrows = nr_ranges, ncols = 1, sharex = True, \
            gridspec_kw={'height_ratios':height_ratios, 'hspace':0})

    for i in range(nr_ranges):
        # Set y axis limits for subplots
        ax[i].set_ylim(ranges[nr_ranges - 1 - i])
        ax[i].set_rasterized(True)
        im = ax[i].pcolorfast(timemap, pagemap, accmap, cmap = 'binary', vmin = 0, vmax = vmax)

        ax[i].set_yticks(ticks[nr_ranges - 1 - i])
        ax[i].locator_params(axis = 'x', nbins = 6)
        if use_hex_address:
            ax[i].get_yaxis().set_major_formatter(ticker.FormatStrFormatter("%x"))
        else:
            if use_plain_address:
                ax[i].get_yaxis().set_major_formatter(ticker.FuncFormatter(\
                        lambda x, p: format(int(x), ',')))
            else:
                ax[i].get_yaxis().set_major_formatter(metric_prefix_formatter)

    fig.suptitle(title, fontsize = 16)
    fig.text(0.5, 0.01, 'Time (seconds)', ha = 'center')
    if show_ytitle:
        fig.text(0.003, 0.56, 'Virtual address space', va = 'center', rotation = 'vertical')
    fig.subplots_adjust(bottom = 0.1, top = 0.9, left = 0.15, right = 1)
    fig.colorbar(im, ax = fig.get_axes(), pad = 0.02, aspect = 20)
    plt.savefig(figname, dpi = 600)
    if interactive_mode:
        plt.show()


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--data', type = str,
            default = 'example_data/example.data',
            help = 'input data file (default: example_data/example.data)')
    parser.add_argument('--name', type = str, default = 'test',
            help = 'name of an experiment (default: test)')
    parser.add_argument('--column', type = int, default = 200,
            help = 'number of columns (default: 200)')
    parser.add_argument('--plain', action = 'store_true',
            help = 'show plain address')
    parser.add_argument('--digits', type = int, default = 5,
            help = 'number of decimal places for address (default: 5)')
    parser.add_argument('--hex', action = 'store_true',
            help = 'show address in hexdecimal')
    parser.add_argument('--vmax', type = int, default = None,
            help = 'set the scale of color')
    parser.add_argument('--adjbot', action = 'store_true',
            help = 'adjust overlapping bottom ticks')
    parser.add_argument('--ytitle', action = 'store_true',
            help = 'show y-axis title')
    parser.add_argument('--interactive', action = 'store_true',
            help = 'show plot in interactive mode')

    return parser.parse_args()


def main():
    simplefilter(action='ignore', category=FutureWarning)

    args = parse_arguments()

    pids = group_by_pid(args.data)
    for pid in pids:
        print "-----------"
        print "pid:" + str(pid)
        print "-----------"
        input_data = '.' + str(pid) + '.data'
        exp_name = args.name + '-' + str(pid)
        data = read_trace_data(input_data, args.column)
        plot(data, exp_name, args.plain, args.digits, args.hex,
                args.vmax, args.adjbot, args.ytitle, args.interactive)

    rm_grouped_files(args.data, pids)


if __name__ == "__main__":
    main()
