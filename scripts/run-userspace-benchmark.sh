
./run-komb-vm.sh stock &

sleep 60

sudo ./pin-vcpu.py

# Run LevelDB

ssh -t -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/benchmarks/leveldb-1.20/; sudo ./run_db_bench.sh'

sleep 5

ssh -t -p 4444 ubuntu@localhost 'sudo shutdown now'

sleep 30

