touch completion.log

echo "Nano-benchmark start" >> completion.log

./run-vm.sh stock &

sleep 60

sudo ./pin-vcpu.py 5555 `nproc`

echo "Run spinlock (Figure 8(a))" >> completion.log

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-spinlock.sh'

sleep 5

echo "Run mutex (Figre 8(c))" >> completion.log

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-mutex.sh'

sleep 5

echo "Run rwsem (Figre 8 (d) & (e))" >> completion.log

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-rwsem.sh'

sleep 5

echo "Run optimizations (Figre 8(f))" >> completion.log

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-optimization.sh'

sleep 5

echo "Run prefetching (Figre 8(g))" >> completion.log

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-prefetch.sh'

sleep 5

echo "Run batch size (Figre 8(h))" >> completion.log

ssh -p 4444 ubuntu@localhost 'cd /home/ubuntu/TCLocks/src/kernel/rcuht;./run-rcuht-batch-size.sh'

sleep 10

echo "Nano-benchmark complete" >> completion.log

sudo pkill -9 qemu
