#!/usr/bin/env python3
import os
from os import listdir
import os.path
import stat
import sys
import subprocess
import optparse
import math
import pdb

CUR_DIR     = os.path.abspath(os.path.dirname(__file__))

"""
# GNUPLOT HOWTO
- http://www.gnuplotting.org/multiplot-placing-graphs-next-to-each-other/
- http://stackoverflow.com/questions/10397750/embedding-multiple-datasets-in-a-gnuplot-command-script
- http://ask.xmodulo.com/draw-stacked-histogram-gnuplot.html
"""

class Plotter(object):
    def __init__(self, log_folder, ncore):
        # config
        self.UNIT_WIDTH  = 2.3
        self.UNIT_HEIGHT = 2.3
        self.PAPER_WIDTH = 7   # USENIX text block width
        self.EXCLUDED_FS = ()  # ("tmpfs")
        self.CPU_UTILS = ["user.util",
                          "sys.util",
                          "idle.util",
                          "iowait.util"]
        self.UNIT = 1000000.0

        # init.
        self.log_folder = log_folder
        self.ncore = ncore
        self.out_dir  = ""
        self.out_file = ""
        self.out = 0

    def _get_pdf_name(self):
        pdf_name = self.out_file
        outs = self.out_file.split(".")
        if outs[-1] == "gp" or outs[-1] == "gnuplot":
            pdf_name = '.'.join(outs[0:-1]) + ".pdf"
        pdf_name = os.path.basename(pdf_name)
        return pdf_name

    def _get_lock_list(self, bench):
        lock_set = set()
        for d in listdir(self.log_folder):
            if str(self.ncore) in d:
                file_path="%s/%s/%s.log" % (self.log_folder, d, bench)
                if os.path.exists(file_path):
                    lock_set.add(d)
        return sorted(list(lock_set))

    def _gen_pdf(self, gp_file):
        subprocess.call("cd %s; gnuplot %s" %
                        (self.out_dir, os.path.basename(gp_file)),
                        shell=True)

    def _plot_header(self, num_bench):
        n_unit = num_bench
        n_col = min(n_unit, int(self.PAPER_WIDTH / self.UNIT_WIDTH))
        n_row = math.ceil(float(n_unit) / float(n_col))
        print("set term pdfcairo size %sin,%sin font \',10\'" %
              (self.UNIT_WIDTH * n_col, self.UNIT_HEIGHT * n_row),
              file=self.out)
        print("set_out=\'set output \"`if test -z $OUT; then echo %s; else echo $OUT; fi`\"\'"
              % self._get_pdf_name(), file=self.out)
        print("eval set_out", file=self.out)
        print("set multiplot layout %s,%s" % (n_row, n_col), file=self.out)
        print("set datafile separator comma", file=self.out)

    def _plot_footer(self):
        print("", file=self.out)
        print("unset multiplot", file=self.out)
        print("set output", file=self.out)


    def _plot_sc_data(self, bench):
        def _get_sc_style(lock):
            return "with lp ps 0.5"

        def _get_data_file(lock):
            return "%s/%s/%s.log" % (self.log_folder,lock, bench)

        # check if there are data
        lock_list = self._get_lock_list(bench)
        if lock_list == []:
            return
 
        # gen gp file
        print("", file=self.out)
        print("set title \'%s\'" % (bench), file=self.out)
        print("set xlabel \'# cores\'", file=self.out)
        print("set ylabel \'%s\'" % "M ops/sec", file=self.out)

        lock = lock_list[0]
        print("plot [0:][-1:] \'%s\' skip 1 using 1:2 title \'%s\' %s"
              % (_get_data_file(lock), lock, _get_sc_style(lock)),
              end="", file=self.out)
        for lock in lock_list[1:]:
            print(", \'%s\' skip 1 using 1:2 title \'%s\' %s"
                  % (_get_data_file(lock), lock, _get_sc_style(lock)),
                  end="", file=self.out)
        print("", file=self.out)

    def plot_sc(self, out_dir):
        self.out_dir  = out_dir
        subprocess.call("mkdir -p %s" % self.out_dir, shell=True)
        self.out_file = os.path.join(self.out_dir, "sc.gp")
        self.out = open(self.out_file, "w")
        num_bench = sum(1 for line in open("bench.txt"))
        self._plot_header(num_bench)
        with open("bench.txt") as fd:
            for bench in fd:
                self._plot_sc_data(bench.strip())
        self._plot_footer()
        self.out.close()
        self._gen_pdf(self.out_file)

def __print_usage():
    print("Usage: plotter.py --log [log folder] ")
    print("                  --gp [gnuplot output]")
    print("                  --ncore [max cores]")

if __name__ == "__main__":
    parser = optparse.OptionParser()
    parser.add_option("--log",   help="Log folder")
    parser.add_option("--out",   help="output directory")
    parser.add_option("--ncore",   help="max cores")
    (opts, args) = parser.parse_args()

    # check arg
    for opt in vars(opts):
        val = getattr(opts, opt)
        if val == None:
            print("Missing options: %s" % opt)
            parser.print_help()
            exit(1)
    # run
    plotter = Plotter(opts.log, opts.ncore)
    plotter.plot_sc(opts.out)
