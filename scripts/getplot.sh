#!/bin/bash

group=$1
workload=$2
np=1

data="data/$group/$workload-1.data"
name="$group.$workload"

mkdir cmd
mkdir fig

cmd="./report.py --data $data --name $name ${@:3}"
echo $cmd > cmd/$name.cmd
eval $cmd

mv $name*.pdf fig/
evince fig/$name*.pdf
