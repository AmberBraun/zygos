/*
 * Copyright 2013-16 Board of Trustees of Stanford University
 * Copyright 2013-16 Ecole Polytechnique Federale Lausanne (EPFL)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * cpu.c - support for multicore and percpu data.
 */

#define _GNU_SOURCE

#include <assert.h>
#include <unistd.h>
#include <sched.h>
#include <sys/syscall.h>
#include <stdio.h>

#include <ix/stddef.h>
#include <ix/errno.h>
#include <ix/log.h>
#include <ix/cpu.h>
#include <ix/mem.h>
#include <ix/mempool.h>

int cpu_count;
int cpus_active;

DEFINE_PERCPU(unsigned int, cpu_numa_node);
DEFINE_PERCPU(unsigned int, cpu_id);
DEFINE_PERCPU(unsigned int, cpu_nr);
DEFINE_PERCPU(unsigned int, apicid);

void *percpu_offsets[NCPU];

extern const char __percpu_start[];
extern const char __percpu_end[];

extern int dune_enter_ex(void *percpu);
#define PERCPU_DUNE_LEN	512

struct cpu_runner {
	struct cpu_runner *next;
	cpu_func_t func;
	void *data;
};

struct cpu_runlist {
	spinlock_t lock;
	struct cpu_runner *next_runner;
} __aligned(CACHE_LINE_SIZE);

static struct apicid_map {
	int processor;
	int apicid;
} apicid_map[NCPU];

static DEFINE_PERCPU(struct cpu_runlist, runlist);

static struct mempool_datastore runners_datastore;

#define MAX_RUNNERS 1024

static DEFINE_PERCPU(struct mempool, runners_mempool);

/**
 * cpu_run_on_one - calls a function on the specified CPU
 * @func: the function to call
 * @data: an argument for the function
 * @cpu: the CPU to run on
 *
 * Returns 0 if successful, otherwise fail.
 */
int cpu_run_on_one(cpu_func_t func, void *data, unsigned int cpu)
{
	struct cpu_runner *runner;
	struct cpu_runlist *rlist;

	if (cpu >= cpu_count)
		return -EINVAL;

	runner = mempool_alloc(&percpu_get(runners_mempool));
	if (!runner)
		return -ENOMEM;

	runner->func = func;
	runner->data = data;
	runner->next = NULL;

	rlist = &percpu_get_remote(runlist, cpu);

	spin_lock(&rlist->lock);
	runner->next = rlist->next_runner;
	rlist->next_runner = runner;
	spin_unlock(&rlist->lock);

	return 0;
}

/**
 * cpu_do_bookkepping - runs periodic per-cpu tasks
 */
void cpu_do_bookkeeping(void)
{
	struct cpu_runlist *rlist = &percpu_get(runlist);
	struct cpu_runner *runner;

	if (rlist->next_runner) {
		spin_lock(&rlist->lock);
		runner = rlist->next_runner;
		rlist->next_runner = NULL;
		spin_unlock(&rlist->lock);

		do {
			struct cpu_runner *last = runner;
			runner->func(runner->data);
			runner = runner->next;
			mempool_free(&percpu_get(runners_mempool), last);
		} while (runner);
	}
}

static void *cpu_init_percpu(unsigned int cpu, unsigned int numa_node)
{
	size_t len = __percpu_end - __percpu_start;
	char *addr, *addr_percpu;

	addr = mem_alloc_pages_onnode(div_up(len + PERCPU_DUNE_LEN, PGSIZE_2MB),
				      PGSIZE_2MB, numa_node, MPOL_BIND);
	if (addr == MAP_FAILED)
		return NULL;

	addr_percpu = addr + PERCPU_DUNE_LEN;

	memset(addr_percpu, 0, len);

	*((char **) addr) = addr_percpu;
	percpu_offsets[cpu] = addr_percpu;

	return addr;
}


/**
 * cpu_init_one - initializes a CPU core
 * @cpu: the CPU core number
 *
 * Typically one should call this right after
 * creating a new thread. Initialization includes
 * binding the thread to the appropriate core,
 * setting up per-cpu memory, and enabling Dune.
 *
 * Returns 0 if successful, otherwise fail.
 */
int cpu_init_one(unsigned int cpu)
{
	int ret, i;
	cpu_set_t mask;
	unsigned int tmp, numa_node;
	void *pcpu;

	if (cpu >= cpu_count)
		return -EINVAL;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	ret = sched_setaffinity(0, sizeof(mask), &mask);
	if (ret)
		return -EPERM;

	ret = syscall(SYS_getcpu, &tmp, &numa_node, NULL);
	if (ret)
		return -ENOSYS;

	if (cpu != tmp) {
		log_err("cpu: couldn't migrate to the correct core\n");
		return -EINVAL;
	}

	pcpu = cpu_init_percpu(cpu, numa_node);
	if (!pcpu)
		return -ENOMEM;

	ret = dune_enter_ex(pcpu);
	if (ret) {
		log_err("cpu: failed to initialize Dune\n");
		return ret;
	}

	percpu_get(cpu_id) = cpu;
	percpu_get(cpu_numa_node) = numa_node;
	for (i = 0; i < NCPU; i++)
		if (apicid_map[i].processor == cpu)
			break;
	assert(i < NCPU);
	percpu_get(apicid) = apicid_map[i].apicid;
	log_is_early_boot = false;

	ret = mempool_create(&percpu_get(runners_mempool), &runners_datastore,
			     MEMPOOL_SANITY_PERCPU, percpu_get(cpu_id));
	if (ret)
		return ret;

	log_info("cpu: started core %d, numa node %d, apicid %d\n", cpu, numa_node, percpu_get(apicid));

	return 0;
}

static int parse_cpuinfo(void)
{
	int ret = 0, processor, apicid, count = 0, tokens;
	FILE *f;
	char buf[BUFSIZ];

	f = fopen("/proc/cpuinfo", "r");
	if (!f)
		return -EIO;

	while (true) {
		if (!fgets(buf, sizeof(buf), f)) {
			if (feof(f))
				break;
			ret = -EIO;
			goto out;
		}

		if (!strncmp(buf, "processor", strlen("processor"))) {
			tokens = sscanf(buf, "%*s : %d", &processor);
			if (tokens != 1) {
				ret = -EIO;
				goto out;
			}
		}

		if (!strncmp(buf, "apicid", strlen("apicid"))) {
			tokens = sscanf(buf, "%*s : %d", &apicid);
			if (tokens != 1) {
				ret = -EIO;
				goto out;
			}

			apicid_map[count].processor = processor;
			apicid_map[count].apicid = apicid;
			count++;
		}
	}

out:
	fclose(f);
	return ret;
}

/**
 * cpu_init - initializes CPU support
 *
 * Returns zero if successful, otherwise fail.
 */
int cpu_init(void)
{
	int ret;

	cpu_count = sysconf(_SC_NPROCESSORS_CONF);

	if (cpu_count <= 0 || cpu_count > NCPU)
		return -EINVAL;

	log_info("cpu: detected %d cores\n", cpu_count);

	ret = mempool_create_datastore(&runners_datastore, MAX_RUNNERS,
		sizeof(struct cpu_runner), 0, MEMPOOL_DEFAULT_CHUNKSIZE,
		"runners");
	if (ret)
		return ret;

	ret = parse_cpuinfo();
	if (ret)
		return ret;

	return 0;
}

