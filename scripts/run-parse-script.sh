
./run-vm.sh &

sleep 60

scp -r -P 4444 ubuntu@localhost:~/TCLocks/doc ../

sudo pkill -9 qemu

export CORES=224

# Nano-benchmark

export LOCK=spinlock
export LOCK_TYPE=spinlock
./rcuht-extract-result.py

export LOCK=mutex
export LOCK_TYPE=mutex
./rcuht-extract-result.py

export LOCK=rwsem
export LOCK_TYPE=rwsem
./rcuht-extract-result.py

export LOCK=spinlock
export LOCK_TYPE=spinlock-optimization
./rcuht-extract-result.py

export LOCK=spinlock
export LOCK_TYPE=spinlock-prefetch
./rcuht-extract-result.py

export LOCK=spinlock
export LOCK_TYPE=spinlock-batch-size
./rcuht-extract-result.py
