
locks=(stock cna shfllock tclocks)

touch completion.log

echo "Micro-benchmark start" >> completion.log

for l in ${locks[@]}
do
	echo ${l}

	./run-vm.sh ${l} &
	sleep 60
	sudo ./pin-vcpu.py 5555 `nproc`

	echo "Running will-it-scale ${l}" >> completion.log
	ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/benchmarks/will-it-scale/;./runscript.sh'
	sleep 5

	echo "Running FxMark ${l}" >> completion.log
	ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/benchmarks/fxmark/;./run-fxmark.sh'
	sleep 10

	sudo pkill -9 qemu

	sleep 30
done

echo "Micro-benchmark end" >> completion.log
