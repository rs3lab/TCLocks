
export cores=(1 2 4 8 12 16 20 28 56 84 112 128 168 224)
export mutex_cores=(1 2 4 8 12 16 20 28 56 84 112 128 168 224 336 448 576 672 800 896)
export ncores=224
export root_dir=/home/ubuntu/TCLocks
export results_dir=${root_dir}/doc/results
export runtime=30
export kernel=`uname -r | sed 's/5.14.16-//'`

## For python-environment
export python_env_cores='[1,2,4,8,12,16,20,28,56,84,112,128,168,224]'
