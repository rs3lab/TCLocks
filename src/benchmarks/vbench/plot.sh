#!/bin/bash

# Wrapper script to generate graphs from the `graphs` python script
# Saves the data and the plot in the folder where the run logs are
# Usage: ./plot.sh <exp_folder> --dir  <output folder>



if [[ $1 == "--dir" ]]; then
    shift
    OUTDIR="${1}"


