
locks=(stock cna shfllock)
locks=(tclocks)

for l in ${locks[@]}
do
	echo ${l}

	./run-vm.sh ${l} &
	sleep 60
	sudo ./pin-vcpu.py

	echo Running will-it-scale ${l}
	ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/benchmarks/will-it-scale/;./runscript.sh'
	sleep 5

	echo Running FxMark ${l}
	ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/benchmarks/fxmark/;./run-fxmark.sh'
	sleep 5

	ssh -t -p 4444 ubuntu@localhost 'sudo shutdown now'
	sleep 30
done

