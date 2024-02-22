KDIR ?= /lib/modules/$(shell uname -r)/build
BDIR ?= $(CURDIR)/build
BDIR_MAKEFILE ?= $(BDIR)/Makefile

STRESSMOD = ht

obj-m += $(STRESSMOD).o

$(STRESSMOD)-objs := rcuht.o

$(STRESSMOD)-objs += locks/qspinlock.o # Linux's qspinlock implementation
$(STRESSMOD)-objs += locks/aqs.o # Our implementation
$(STRESSMOD)-objs += locks/aqm.o # Our implementation

$(STRESSMOD)-objs += locks/rwaqs_ntrl.o
$(STRESSMOD)-objs += locks/rwaqs_rp.o
$(STRESSMOD)-objs += locks/rwaqs_rp_v1.o
$(STRESSMOD)-objs += locks/cna.o
$(STRESSMOD)-objs += locks/rwaqm.o

$(STRESSMOD)-objs += locks/komb.o
#$(STRESSMOD)-objs += locks/komb_rwlock.o
#$(STRESSMOD)-objs += locks/komb_mutex.o
#$(STRESSMOD)-objs += locks/komb_rwsem.o
$(STRESSMOD)-objs += timing_stats.o
$(STRESSMOD)-objs += lib/combiner.o

$(STRESSMOD)-objs += locks/swilock_delegate.o

ccflags-y := -g -DDEBUG -DKERNEL_SYNCSTRESS -I$(src)/include/ -I$(src)/spinlock/ -Werror

Q	= @

all: $(BDIR_MAKEFILE) CPUSEQ
	make -C $(KDIR) M=$(BDIR) src=$(CURDIR) modules

CPUSEQ:
	$(Q)./script/gen-cpuseq.py

$(BDIR_MAKEFILE): $(BDIR)
	$(Q)touch $(BDIR_MAKEFILE)

$(BDIR):
	$(Q)mkdir -p $(BDIR)/locks

clean:
	$(Q)rm -rf build
	$(Q)rm -f include/cpuseq.h
	sed -i 's/NUM_PREFETCH_LINES ./NUM_PREFETCH_LINES 6/' include/lib/combiner.h 
