#!/usr/bin/python3

# Copyright (c) 2020, Stanford University
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

# This script generates a plot of slowdown as a function of message
# length, with an x-axis scaled to match the CDF of message lengths.
#
# Usage: plot_slowdown.py log_dir [title]
#
# "log_dir" is the name of a directory containing log files created by
# "cperf slowdown". "title" is an optional title for the graph.

import glob
import math
import matplotlib.pyplot as plt
import numpy as np
import os
import string
import sys

def read_dump(file, data):
    """
    Read file (generated by cp_node's "dump_times" command) and add its
    data to the information present in data (which is either an empty
    dictionary or the result of the previous call to this method.
    """

    if not "rtts" in data:
        data["rtts"] = {}
    rtts = data["rtts"]
    if not "counts" in data:
        data["counts"] = {}
    counts = data["counts"]
    if "total_messages" in data:
        total = data["total_messages"]
    else:
        total = 0
    f = open(file, "r")
    for line in f:
        stripped = line.strip();
        if stripped[0] == '#':
            continue
        words = stripped.split()
        if (len(words) < 2):
            print("Line too short (need at least 2 columns): '%s'" % (line))
            continue
        length = int(words[0])
        usec = float(words[1])
        if length in counts:
            counts[length] += 1
            rtts[length].append(usec)
        else:
            counts[length] = 1
            rtts[length] = [usec]
        total += 1
    f.close()
    data["total_messages"] = total

def get_buckets(data):
    """
    Computes how large the buckets should be for histogramming the information
    in "data". We don't want super-small buckets because the statistics will
    be bad, so the computed buckets may merge several message sizes to create
    larger buckets. The return value is a list of <length, cum_frac> pairs,
    in sorted order. The length is the largest message size for a bucket,
    and cum_frac is the fraction of all messages with that length or smaller.
    """
    buckets = []
    counts = data["counts"]
    total = data["total_messages"]
    min_size = total//400
    cur_size = 0
    cumulative = 0
    for length in sorted(counts.keys()):
        cur_size += counts[length]
        cumulative += counts[length]
        if cur_size >= min_size:
            buckets.append([length, cumulative/total])
            cur_size = 0
        last_length = length
    if cur_size != 0:
        buckets[-1] = [last_length, 1.0]
    return buckets

def digest(data, unloaded, buckets):
    """
    The "data" argument contains data produced by read_dump, "unloaded"
    is a dictionary mapping message lengths to the unloaded RTT for that
    length, and "buckets" is output from get_buckets. This method adds new
    fields to data:
    lengths:    sorted list of message lengths, corresponding to buckets
                for plotting
    cum_frac:   cumulative fraction of all messages corresponding to each length
    counts:     number of RTTs represented by each bucket
    p50:        list of 50th percentile rtts corresponding to each length
    p99:        list of 99th percentile rtts corresponding to each length
    p999:       list of 999th percentile rtts corresponding to each length
    slow_50:    list of 50th percentile slowdowns corresponding to each length
    slow_99:    list of 99th percentile slowdowns corresponding to each length
    slow_999:   list of 999th percentile slowdowns corresponding to each length
    """
    data["lengths"] = []
    data["cum_frac"] = []
    data["counts"] = []
    data["p50"] = []
    data["p99"] = []
    data["p999"] = []
    data["slow_50"] = []
    data["slow_99"] = []
    data["slow_999"] = []

    rtts = data["rtts"]
    bucket_length, bucket_cum_frac = buckets[0]
    next_bucket = 1
    bucket_rtts = []
    bucket_slowdowns = []
    bucket_count = 0
    cur_unloaded = unloaded[min(unloaded.keys())]
    lengths = sorted(rtts.keys())
    lengths.append(999999999)            # Force one extra loop iteration
    for length in lengths:
        if length > bucket_length:
            data["lengths"].append(bucket_length)
            data["cum_frac"].append(bucket_cum_frac)
            data["counts"].append(bucket_count)
            if len(bucket_rtts) == 0:
                bucket_rtts.append(0)
                bucket_slowdowns.append(0)
            bucket_rtts = sorted(bucket_rtts)
            data["p50"].append(bucket_rtts[bucket_count//2])
            data["p99"].append(bucket_rtts[bucket_count*99//100])
            data["p999"].append(bucket_rtts[bucket_count*999//1000])
            bucket_slowdowns = sorted(bucket_slowdowns)
            data["slow_50"].append(bucket_slowdowns[bucket_count//2])
            data["slow_99"].append(bucket_slowdowns[bucket_count*99//100])
            data["slow_999"].append(bucket_slowdowns[bucket_count*999//1000])
            if next_bucket >= len(buckets):
                break
            bucket_rtts = []
            bucket_slowdowns = []
            bucket_count = 0
            bucket_length, bucket_cum_frac = buckets[next_bucket]
            next_bucket += 1
        if length in unloaded:
            cur_unloaded = unloaded[length]
        bucket_count += len(rtts[length])
        for rtt in rtts[length]:
            bucket_rtts.append(rtt)
            bucket_slowdowns.append(rtt/cur_unloaded)

def make_histogram(x, y):
    """
    Given x and y coordinates, return new lists of coordinates that describe
    a histogram (transform (x1,y1) and (x2,y2) into (x1,y1), (x2,y1), (x2,y2)
    to make steps. The arguments are lists of x and y values, and the
    result is a list containing two lists, one with new x values and one
    with new y values.
    """
    x_new = []
    y_new = []
    for i in range(len(x)):
        if len(x_new) != 0:
            x_new.append(x[i])
            y_new.append(y[i-1])
        else:
            x_new.append(0)
            y_new.append(y[i])
        x_new.append(x[i])
        y_new.append(y[i])
    return [x_new, y_new]

def get_short_cdf(data):
    """
    Given data collected by read_dump, extract out all RTTs for messages
    shorter than 1500 bytes that are also among the 10% of shortest
    messages. Return a list with two elements (a list of x-coords and a
    list of y-coords) that histogram the complementary cdf.
    """
    short = []
    rtts = data["rtts"]
    messages_left = data["total_messages"]//10
    for length in sorted(rtts.keys()):
        if length >= 1500:
            break
        short.extend(rtts[length])
        messages_left -= len(rtts[length])
        if messages_left < 0:
            break
    x = []
    y = []
    total = len(short)
    remaining = total
    for rtt in sorted(short):
        if len(x) > 0:
            x.append(rtt)
            y.append(remaining/total)
        remaining -= 1
        x.append(rtt)
        y.append(remaining/total)
    return [x, y]

title = ""
if len(sys.argv) == 3 :
    title = sys.argv[2]
elif len(sys.argv) != 2:
    print("Usage: %s name log_dir [title]" % (sys.argv[0]))
    exit(1)
log_dir = sys.argv[1]

print("Reading unloaded data")
unloaded = {}
read_dump(log_dir + "/unloaded.txt", unloaded)
unloaded_rtts = {}
lengths = sorted(unloaded["rtts"].keys())
for length in lengths:
    rtts = unloaded["rtts"][length]
    unloaded_rtts[length] = sorted(rtts)[len(rtts)//2]

print("Reading Homa data")
homa = {}
for file in sorted(glob.glob(log_dir + "/loaded-*.txt")):
    print("Reading data from %s" % (file))
    read_dump(file, homa)
buckets = get_buckets(homa)
digest(homa, unloaded_rtts, buckets)

print("Reading TCP data")
tcp = {}
for file in sorted(glob.glob(log_dir + "/tcp-*.txt")):
    print("Reading data from %s" % (file))
    read_dump(file, tcp)  
digest(tcp, unloaded_rtts, buckets)

print("# length cum_frac   homa_count homa_p50 homa_s50 homa_p99 homa_s99 " \
        "homa_p999 homa_s999    tcp_count tcp_p50 tcp_s50 tcp_p99 tcp_s99 " \
        "tcp_p999 tcp_s999")
for i in range(len(homa["lengths"])):
    line = "%7d %8.3f  %10d %8.1f %8.1f %8.1f %8.1f  %9.1f %8.1f   %10d %7.1f %7.1f % 8.1f " \
            "%7.1f %8.1f %8.1f" % (homa["lengths"][i], homa["cum_frac"][i],
            homa["counts"][i], homa["p50"][i], homa["slow_50"][i],
            homa["p99"][i], homa["slow_99"][i], homa["p999"][i],
            homa["slow_999"][i], tcp["counts"][i], tcp["p50"][i],
            tcp["slow_50"][i], tcp["p99"][i], tcp["slow_99"][i],
            tcp["p999"][i], tcp["slow_999"][i])
    print(line)

#--------------- Slowdown plot -----------------------

plt.figure(figsize=[6, 3])
if title != "":
    plt.title(title)
plt.rcParams.update({'font.size': 10})
plt.axis()
plt.xlim(0, 1.0)
plt.yscale("log")
plt.ylim(1, 100)
plt.yticks([1, 10, 100], ["1", "10", "100"])
plt.xlabel("Message Length")
plt.ylabel("Slowdown")
plt.grid(which="major", axis="y")

# Generate x-axis labels
xticks = []
xlabels = []
cum_frac = 0.0
target = 0.0
rtts = homa["rtts"]
total = homa["total_messages"]
for length in sorted(rtts.keys()):
    cum_frac += len(rtts[length])/total
    while cum_frac >= target:
        xticks.append(target)
        if length < 1000:
            xlabels.append("%.0f" % (length))
        elif length < 100000:
            xlabels.append("%.1fK" % (length/1000))
        else:
            xlabels.append("%.0fK" % (length/1000))
        target += 0.1
plt.xticks(xticks, xlabels)

x, y = make_histogram(homa["cum_frac"], homa["slow_50"])
plt.plot(x, y, label="Homa P50")
x, y = make_histogram(homa["cum_frac"], homa["slow_99"])
plt.plot(x, y, label="Homa P99")
x, y = make_histogram(tcp["cum_frac"], tcp["slow_50"])
plt.plot(x, y, label="TCP P50")
x, y = make_histogram(tcp["cum_frac"], tcp["slow_99"])
plt.plot(x, y, label="TCP P99")
plt.legend()
plt.tight_layout()

plt.savefig("%s/slowdown.pdf" % (log_dir))

#--------------- CDF of small message times -----------------------

homa_x, homa_y = get_short_cdf(homa)
print("Homa short message CDF: %d points (out of %d), min %.1f, max %.1f" % (
        len(homa_x)//2 + 1, homa["total_messages"], homa_x[0], homa_x[-1]))
tcp_x, tcp_y = get_short_cdf(tcp)
print("TCP short message CDF: %d points (out of %d), min %.1f, max %.1f" % (
        len(tcp_x)//2 + 1, tcp["total_messages"], tcp_x[0], tcp_x[-1]))
unloaded_x, unloaded_y = get_short_cdf(unloaded)
print("Unloaded short message CDF: %d points (out of %d), min %.1f, max %.1f" % (
        len(unloaded_x)//2 + 1, unloaded["total_messages"],
        unloaded_x[0], unloaded_x[-1]))

plt.figure(figsize=[5, 4])
if title != "":
    plt.title(title)
plt.rcParams.update({'font.size': 10})
plt.axis()
plt.xscale("log")

# Round out the x-axis limits to even powers of 10.
xmin = min(homa_x[0], tcp_x[0], unloaded_x[0])
exp = math.floor(math.log(xmin, 10))
xmin = 10**exp
xmax = max(homa_x[-1], tcp_x[-1], unloaded_x[-1])
exp = math.ceil(math.log(xmax, 10))
xmax = 10**exp
plt.xlim(xmin, xmax)

plt.yscale("log")
plt.ylim(1/(max(len(homa_x), len(homa_y))/2 + 1), 1.0)
# plt.yticks([1, 10, 100, 1000], ["1", "10", "100", "1000"])
plt.xlabel("RTT (usecs)")
plt.ylabel("Cumulative Fraction of Short Messages")
plt.grid(which="major", axis="y")
plt.grid(which="major", axis="x")

plt.plot(tcp_x, tcp_y, label="TCP")
plt.plot(homa_x, homa_y, label="Homa")
plt.plot(unloaded_x, unloaded_y, label="Homa low load")
plt.legend(loc="upper right")

plt.savefig("%s/short_cdf.pdf" % (log_dir))