
locks=(stock cna shfllock komb)

touch completion.log

echo "Macro-benchmark start" >> completion.log

for l in ${locks[@]}
do
	./run-vm.sh ${l} &

	sleep 60

	sudo ./pin-vcpu.py 5555 `nproc`

	echo "Run Metis and Psearchy $l" >> completion.log

	ssh -t -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/benchmarks/vbench/; sudo ./run.sh'

	sleep 10

	sudo pkill -9 qemu

	sleep 30
done

echo "Macro-benchmark end" >> completion.log
