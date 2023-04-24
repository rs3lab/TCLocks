#!/usr/bin/perl

require "/home/ubuntu/kcombiner/src/benchmarks/vbench/metis/test/comsubs.pl";

my $opts = "-p 1";


do_test("hist", "data/hist-5.2g.bmp $opts");
