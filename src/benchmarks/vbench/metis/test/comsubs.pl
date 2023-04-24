#!/usr/bin/perl

my $sfpath = '/home/ubuntu/kcombiner/src/benchmarks/vbench/metis/obj/lib/';
my $metispath = '/home/ubuntu/kcombiner/src/benchmarks/vbench/metis/';
my $hugetlbfs = "/mnt/huge";

sub do_test {
    my $appname = shift;
    my $args = shift;
    system("rm $hugetlbfs/* >/dev/null 2>&1");
    
    my $cmd ="$sfpath ./obj/app/$appname.sf $args";
    #print "$cmd\n";
    #system($cmd);
    #print "\n";
    my $cmd1 ="${metispath}obj/app/${appname}";
    print "$cmd1\n";
    system("$cmd1 $args");
    #system("./obj/app/$appname $args");
};

1;
