
locks=(stock cna shfllock komb)

for l in ${locks[@]}
do
	./run-vm.sh ${l} &

	sleep 60

	sudo ./pin-vcpu.py

	# Run Metis and Psearchy 

	ssh -t -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/benchmarks/vbench/; sudo ./run.sh'

	sleep 5

	ssh -t -p 4444 ubuntu@localhost 'sudo shutdown now'

	sleep 30
done

