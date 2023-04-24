
touch completion.log

echo "Userspace benchmark start" >> completion.log
./run-vm.sh stock &

sleep 60

sudo ./pin-vcpu.py 5555 `nproc`

# Run LevelDB

ssh -t -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/benchmarks/leveldb-1.20/; sudo ./run_db_bench.sh'

sleep 10

echo  "Userspace benchmark end" >> completion.log

sudo pkill -9 qemu
