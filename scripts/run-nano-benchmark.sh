
./run-vm.sh stock &

sleep 60

sudo ./pin-vcpu.py 5555 `nproc`

# Run spinlock (Figure 8(a)) 

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-spinlock.sh'

sleep 5

# Run mutex (Figre 8(c))

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-mutex.sh'

sleep 5

# Run rwsem (Figre 8 (d) & (e))

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-rwsem.sh'

sleep 5

# Run optimizations (Figre 8(f))

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-optimization.sh'

sleep 5

# Run prefetching (Figre 8(g))

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-prefetching.sh'

sleep 5

# Run batch size (Figre 8(h))

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-batch-size.sh'

sleep 5

ssh -t -p 4444 ubuntu@localhost 'sudo shutdown now'

