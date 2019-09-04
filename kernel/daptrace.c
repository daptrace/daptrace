// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "mapia: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mmu_notifier.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/mm.h>

#define mapia_last_region(t) \
	(container_of(t->regions_list.prev, struct mapia_region, list))

#define mapia_next_region(r) \
	(container_of(r->list.next, struct mapia_region, list))

#define mapia_prev_region(r) \
	(container_of(r->list.prev, struct mapia_region, list))

#define mapia_for_each_region(r, t) \
	list_for_each_entry(r, &t->regions_list, list)

#define mapia_for_each_region_safe(r, next, t) \
	list_for_each_entry_safe(r, next, &t->regions_list, list)

#define mapia_for_each_task(t) \
	list_for_each_entry(t, &tasks_list, list)

#define mapia_for_each_task_safe(t, next) \
	list_for_each_entry_safe(t, next, &tasks_list, list)

struct mapia_region {
	unsigned long vm_start;
	unsigned long vm_end;
	unsigned long sampling_addr;
	unsigned int nr_accesses;
	struct list_head list;
};

struct mapia_task {
	unsigned long pid;
	struct list_head regions_list;	/* list of mapia_region objects */
	struct list_head list;		/* head for tasks list */
};

/* List of mapia_task objects */
static LIST_HEAD(tasks_list);

/* Nano-seconds */
static unsigned long sample_interval = 1000;
static unsigned long aggr_interval = 1000 * 1000;
static unsigned long regions_update_interval = 1000 * 1000 * 100;

static struct timespec64 last_aggregate_time;
static struct timespec64 last_regions_update_time;

static unsigned long min_nr_regions = 10;
static unsigned long max_nr_regions = 1000;

bool mapia_tracing_on;

static unsigned long mapia_merge_threshold = 100;	/* 10 percent */

struct rnd_state rndseed;

/* result buffer */
#define MAPIA_LEN_RBUF	(1024 * 1024 * 4)
static char mapia_rbuf[MAPIA_LEN_RBUF];
static unsigned int mapia_rbuf_offset;

/* result file */
#define LEN_RES_FILE_PATH	256
/* TODO: Make this path configurable */
static char rfile_path[LEN_RES_FILE_PATH] = "/mapia.res.bin";
static struct file *rfile;

static struct task_struct *mapia_trace_task;

static inline unsigned long mapia_sampling_target(struct mapia_region *r)
{
	return r->vm_start + prandom_u32_state(&rndseed) %
		(r->vm_end - r->vm_start);
}

/*
 * Return new region object
 */
static struct mapia_region *mapia_new_region(unsigned long vm_start,
					unsigned long vm_end)
{
	struct mapia_region *ret;

	ret = kmalloc(sizeof(struct mapia_region), GFP_KERNEL);
	if (ret == NULL)
		return NULL;
	ret->vm_start = vm_start;
	ret->vm_end = vm_end;
	ret->nr_accesses = 0;
	ret->sampling_addr = mapia_sampling_target(ret);
	INIT_LIST_HEAD(&ret->list);

	return ret;
}

static inline void __mapia_add_region(struct mapia_region *r,
		struct mapia_region *prev, struct mapia_region *next)
{
	__list_add(&r->list, &prev->list, &next->list);
}

/*
 * Add a region into a task
 */
static void mapia_add_region(struct mapia_region *r, struct mapia_task *t)
{
	list_add_tail(&r->list, &t->regions_list);
}

/*
 * Remove a region from a task
 *
 * NOTE: It just remove the region from the list, does not de-allocate the
 * region
 */
static void mapia_rm_region(struct mapia_region *r)
{
	list_del(&r->list);
}

/*
 * Destroy a region
 */
static void mapia_destroy_region(struct mapia_region *r)
{
	kfree(r);
}

static struct mapia_task *mapia_new_task(unsigned long pid)
{
	struct mapia_task *t;

	t = kmalloc(sizeof(struct mapia_task), GFP_KERNEL);
	if (t == NULL)
		return NULL;
	t->pid = pid;
	INIT_LIST_HEAD(&t->regions_list);

	return t;
}

/* Get nth mapia region */
struct mapia_region *mapia_nth_region_of(struct mapia_task *t, unsigned int n)
{
	struct mapia_region *r;
	unsigned int i;

	i = 0;
	mapia_for_each_region(r, t) {
		if (i++ == n)
			return r;
	}
	return NULL;
}

static void mapia_add_task(struct mapia_task *t)
{
	list_add_tail(&t->list, &tasks_list);
}

static void mapia_rm_task(struct mapia_task *t)
{
	list_del(&t->list);
}

static void mapia_destroy_task(struct mapia_task *t)
{
	struct mapia_region *r, *next;

	mapia_for_each_region_safe(r, next, t) {
		mapia_rm_region(r);
		mapia_destroy_region(r);
	}
	kfree(t);
}

static long mapia_set_pids(unsigned long *pids, unsigned long nr_pids)
{
	unsigned long i;
	struct mapia_task *t, *next;
	bool found;

	/* Remove unselected tasks */
	mapia_for_each_task_safe(t, next) {
		found = false;
		for (i = 0; i < nr_pids; i++) {
			if (pids[i] == t->pid) {
				found = true;
				break;
			}
		}
		if (found)
			continue;
		mapia_rm_task(t);
		mapia_destroy_task(t);
	}

	/* Add new tasks */
	for (i = 0; i < nr_pids; i++) {
		found = false;
		mapia_for_each_task(t) {
			if (t->pid == pids[i]) {
				found = true;
				break;
			}
		}
		if (found)
			continue;
		t = mapia_new_task(pids[i]);
		if (t == NULL) {
			pr_err("Failed to alloc mapia_task\n");
			return -ENOMEM;
		}
		mapia_add_task(t);
	}

	return 0;
}

static long mapia_set_attrs(unsigned long sample_int,
		unsigned long aggr_int, unsigned long regions_update_int,
		unsigned long min_nr_reg, unsigned long max_nr_reg,
		char *path_to_rfile)
{
	if (strnlen(path_to_rfile, LEN_RES_FILE_PATH) >= LEN_RES_FILE_PATH) {
		pr_err("too long (>%d) result file path %s\n",
				LEN_RES_FILE_PATH, path_to_rfile);
		return -EINVAL;
	}
	sample_interval = sample_int;
	aggr_interval = aggr_int;
	regions_update_interval = regions_update_int;
	min_nr_regions = min_nr_reg;
	max_nr_regions = max_nr_reg;
	strncpy(rfile_path, path_to_rfile, LEN_RES_FILE_PATH);
	return 0;
}

struct region {
	unsigned long start;
	unsigned long end;
};

static unsigned long sz_region(struct region *r)
{
	return r->end - r->start;
}

static void swap_regions(struct region *r1, struct region *r2)
{
	struct region tmp;

	tmp = *r1;
	*r1 = *r2;
	*r2 = tmp;
}

/*
 * Find three regions in a list of vmas
 *
 * vma		head of the list of virtual memory areas that illustrating
 *		entire address space
 * regions	array of three 'struct region' that result will be saved
 *
 * Find three regions seperated by two biggest gaps in given address space,
 * which is constructed with given list of virtual memory areas.  Because gaps
 * between stack, memory mapped regions and heap are usually very huge, those
 * would be the two gaps in usual case.
 */
static void mapia_three_regions_in_vmas(struct vm_area_struct *vma,
		struct region regions[3])
{
	struct vm_area_struct *prev_vma = NULL;
	struct region gap = {0,}, first_gap = {0,}, second_gap = {0,};
	unsigned long start = 0, end = 0;

	/* Find two biggest gaps */
	for (; vma; vma = vma->vm_next) {
		if (prev_vma == NULL) {
			start = vma->vm_start;
			prev_vma = vma;
			continue;
		}
		if (vma->vm_next == NULL)
			end = vma->vm_end;
		gap.start = prev_vma->vm_end;
		gap.end = vma->vm_start;
		/*
		 * size of gaps should: gap < second_gap < first_gap.
		 * We use bubble-sort like algorithm with two bubbles.
		 */
		if (sz_region(&gap) > sz_region(&second_gap)) {
			swap_regions(&gap, &second_gap);
			if (sz_region(&second_gap) > sz_region(&first_gap))
				swap_regions(&second_gap, &first_gap);
		}
		prev_vma = vma;
	}

	/* Sort by address order */
	if (first_gap.start > second_gap.start)
		swap_regions(&first_gap, &second_gap);

	/* Store the result */
	regions[0].start = start;
	regions[0].end = first_gap.start;
	regions[1].start = first_gap.end;
	regions[1].end = second_gap.start;
	regions[2].start = second_gap.end;
	regions[2].end = end;
}

/*
 * Get three big regions in given task
 *
 * Returns 0 or negative error value if failed
 */
static int mapia_three_regions_of(struct mapia_task *t,
				struct region regions[3])
{
	struct task_struct *task;
	struct mm_struct *mm;

	rcu_read_lock();
	task = pid_task(find_vpid(t->pid), PIDTYPE_PID);
	if (!task)
		goto err;

	/* TODO: get/put mm */
	mm = task->mm;
	if (!mm) {
		rcu_read_unlock();
		return -EINVAL;
	}

	down_read(&mm->mmap_sem);
	mapia_three_regions_in_vmas(mm->mmap, regions);
	up_read(&mm->mmap_sem);
	rcu_read_unlock();

	return 0;

err:
	rcu_read_unlock();
	return -EINVAL;
}

/*
 * Split a region into 'nr_slips' slips having same size
 */
static void mapia_split_region_n(struct mapia_region *r, unsigned int nr_slips)
{
	unsigned long sz_orig, sz_slip, orig_end;
	struct mapia_region *slip = NULL, *next;
	unsigned long start;

	orig_end = r->vm_end;
	sz_orig = r->vm_end - r->vm_start;
	sz_slip = PAGE_ALIGN(DIV_ROUND_UP(sz_orig, nr_slips));

	r->vm_end = r->vm_start + sz_slip;
	next = mapia_next_region(r);
	for (start = r->vm_end; start < orig_end; start += sz_slip) {
		slip = mapia_new_region(start, start + sz_slip);
		__mapia_add_region(slip, r, next);
		r = slip;
	}
	if (slip)
		slip->vm_end = orig_end;
}

/*
 * Initialize 'regions_list' of given task
 *
 * Usual memory map of normal processes is as below:
 *
 *   <code>
 *   <heap>
 *   <big gap>
 *   <file-backed or anonymous pages>
 *   <big gap>
 *   <stack>
 *   <vvar>
 *   <vdso>
 *   <vsyscall>
 *
 * This function seperates the virtual address space with three regions
 * seperated by the two big gaps.
 */
static void init_regions_of(struct mapia_task *t)
{
	struct mapia_region *r;
	struct region regions[3];
	int ret, i;

	ret = mapia_three_regions_of(t, regions);
	if (ret)
		return;

	/* Set the initial three regions of the task */
	for (i = 0; i < 3; i++) {
		r = mapia_new_region(regions[i].start, regions[i].end);
		mapia_add_region(r, t);
	}

	r = mapia_nth_region_of(t, 1);
	if (!r) {
		pr_err("Initialization didn't made three regions?\n");
		return;
	}
	mapia_split_region_n(r, min_nr_regions - 2);
}

/* Initialize 'mapia_region' data structures for every tasks */
static void init_regions(void)
{
	struct mapia_task *t;

	mapia_for_each_task(t)
		init_regions_of(t);
}

/*
 * Check access to given region
 *
 * t	task of given region
 * r	region to be checked
 *
 * This function checks whether given virtual address space region of given
 * task has been accessed since last check.  In detail, it uses the page table
 * entry access bit for a page in the region that randomly selected.
 */
static void check_access(struct mapia_task *t, struct mapia_region *r)
{
	unsigned long target_addr;
	struct task_struct *task;
	struct mm_struct *mm;
	struct mmu_notifier_range range;
	pte_t *pte = NULL;
	pmd_t *pmd = NULL;
	spinlock_t *ptl;
	int ret;

	/* rcu for task */
	rcu_read_lock();
	task = pid_task(find_vpid(t->pid), PIDTYPE_PID);
	if (!task)
		return;

	/* TODO: mm should be get/put */
	mm = task->mm;
	rcu_read_unlock();
	if (!mm)
		return;

	target_addr = r->sampling_addr;

	ret = follow_pte_pmd(mm, target_addr, &range, &pte, &pmd, &ptl);
	if (ret)
		goto mkold;

	/* Manipulate the access bit of the page */
	if (pmd) {
		if (pmd_young(*pmd))
			r->nr_accesses++;
	} else {
		if (pte_young(*pte))
			r->nr_accesses++;
		pte_unmap(pte);
	}

	spin_unlock(ptl);

mkold:

	/* mkold next target */
	r->sampling_addr = mapia_sampling_target(r);
	target_addr = r->sampling_addr;

	ret = follow_pte_pmd(mm, target_addr, NULL, &pte, &pmd, &ptl);
	if (ret)
		return;

	/* Manipulate the access bit of the page */
	if (pmd)
		*pmd = pmd_mkold(*pmd);
	else
		*pte = pte_mkold(*pte);

	spin_unlock(ptl);
}

/*
 * Check whether given time interval is elapsed
 *
 * See whether the time has passed since given baseline for given interval.  If
 * so, it also set the baseline as current time for later check.
 *
 * Returns true if the time has elapsed, or false.
 */
static bool mapia_check_time_interval_reset(struct timespec64 *baseline,
		unsigned long interval)
{
	struct timespec64 now;

	ktime_get_coarse_ts64(&now);
	if (timespec64_to_ns(&now) - timespec64_to_ns(baseline) < interval)
		return false;
	*baseline = now;
	return true;
}

/*
 * Check whether it is time to aggregate samples
 *
 * Returns true if it is, false else.
 */
static bool need_aggregate(void)
{
	return mapia_check_time_interval_reset(&last_aggregate_time,
			aggr_interval);
}

/*
 * Flush the content in result buffer to file
 */
static void mapia_flush_rbuffer(void)
{
	ssize_t sz;
	loff_t pos = 0;

	rfile = filp_open(rfile_path, O_CREAT | O_RDWR | O_APPEND, 0644);
	if (IS_ERR(rfile)) {
		pr_err("Failed to open result file\n");
		return;
	}

	sz = kernel_write(rfile, mapia_rbuf, mapia_rbuf_offset, &pos);
	if (sz != mapia_rbuf_offset)
		pr_err("failed to flush rbuf at once.. errno: %zd\n", sz);
	filp_close(rfile, NULL);

	mapia_rbuf_offset = 0;
}

/* Store data into the result buffer */
static void mapia_write_rbuf(void *data, ssize_t size)
{
	/* TODO: Flush buffer to a file */
	if (mapia_rbuf_offset + size > MAPIA_LEN_RBUF)
		mapia_flush_rbuffer();

	memcpy(&mapia_rbuf[mapia_rbuf_offset], data, size);
	mapia_rbuf_offset += size;
}

static unsigned int nr_mapia_tasks(void)
{
	struct mapia_task *t;
	unsigned int ret = 0;

	mapia_for_each_task(t)
		ret++;
	return ret;
}

/*
 * Return number of regions for given process
 *
 * TODO: Optimization?
 */
static unsigned int nr_mapia_regions(struct mapia_task *t)
{
	struct mapia_region *r;
	unsigned int ret = 0;

	mapia_for_each_region(r, t)
		ret++;
	return ret;
}

/*
 * Merge two adjacent regions into one region
 */
static void mapia_merge_two_regions(struct mapia_region *l,
				struct mapia_region *r)
{
	BUG_ON(mapia_next_region(l) != r);

	l->vm_end = r->vm_end;
	l->nr_accesses = (l->nr_accesses + r->nr_accesses) / 2;
	mapia_rm_region(r);
	mapia_destroy_region(r);
}

/*
 * merge adjacent regions having similar nr_accesses
 *
 * t		task that merge operation will make change
 * thres	threshold for similarity decision
 *
 * TODO: In case of adjacent regions having nr_accesses 0 and 1, should we
 * don't merge these regions?  Current algorithm will don't merge.  In sense of
 * human eye, those should be merged!
 */
static void mapia_merge_regions_of(struct mapia_task *t, unsigned int thres)
{
	struct mapia_region *r, *prev = NULL, *next;
	unsigned long diff, avg;

	mapia_for_each_region_safe(r, next, t) {
		if (prev == NULL)
			goto next;
		if (prev->vm_end != r->vm_start)
			goto next;
		avg = (prev->nr_accesses + r->nr_accesses) / 2;
		/* If average is zero, two nr_accesses are zero */
		if (!avg)
			goto merge;
		diff = r->nr_accesses > avg ?
			r->nr_accesses - avg : avg - r->nr_accesses;
		if (diff * 2 * 1000 / avg > thres)
			goto next;
merge:
		mapia_merge_two_regions(prev, r);
		continue;
next:
		prev = r;
	}
}

static void mapia_merge_regions(void)
{
	struct mapia_task *t;
	unsigned int nr_regions = 0;

	mapia_for_each_task(t)
		nr_regions += nr_mapia_regions(t);
	if (nr_regions < min_nr_regions * 2)
		return;

	mapia_for_each_task(t)
		mapia_merge_regions_of(t, mapia_merge_threshold);
}

/*
 * Split a region into two regions
 *
 * sz_r	size of the first splitted region
 */
static void mapia_split_region_at(struct mapia_region *r, unsigned long sz_r)
{
	struct mapia_region *new, *next;

	new = mapia_new_region(r->vm_start + sz_r, r->vm_end);
	r->vm_end = new->vm_start;
	r->sampling_addr = mapia_sampling_target(r);

	next = mapia_next_region(r);
	__mapia_add_region(new, r, next);
}

static void mapia_split_regions_of(struct mapia_task *t)
{
	struct mapia_region *r, *next;
	unsigned long nr_pages_region, nr_pages_left_region;
	unsigned long sz_left_region;

	mapia_for_each_region_safe(r, next, t) {
		nr_pages_region = (r->vm_end - r->vm_start) / PAGE_SIZE;
		if (nr_pages_region == 1)
			continue;
		nr_pages_left_region = prandom_u32_state(&rndseed) %
				(nr_pages_region - 1) + 1;
		sz_left_region = nr_pages_left_region * PAGE_SIZE;

		mapia_split_region_at(r, sz_left_region);
	}
}

static void mapia_split_regions(void)
{
	struct mapia_task *t;
	unsigned int nr_regions = 0;

	mapia_for_each_task(t)
		nr_regions += nr_mapia_regions(t);
	if (nr_regions > max_nr_regions / 2)
		return;

	mapia_for_each_task(t)
		mapia_split_regions_of(t);
}

/*
 * Aggregate samples to one set of memory access patterns for each region
 *
 * In detail, this function stores current tracking results to the result
 * buffer and reset nr_accesses of each regions.  The format for the result
 * buffer is as below:
 *
 * <time><number of tasks><array of task infos>
 * task info: <pid><number of regions><array of region infos>
 * region info: <start address><end address><nr_accesses>
 */
static void aggregate(void)
{
	struct mapia_task *t;
	struct timespec64 now;
	unsigned int nr;

	ktime_get_coarse_ts64(&now);

	mapia_write_rbuf(&now, sizeof(struct timespec64));
	nr = nr_mapia_tasks();
	mapia_write_rbuf(&nr, sizeof(nr));

	mapia_for_each_task(t) {
		struct mapia_region *r;

		mapia_write_rbuf(&t->pid, sizeof(t->pid));
		nr = nr_mapia_regions(t);
		mapia_write_rbuf(&nr, sizeof(nr));
		mapia_for_each_region(r, t) {
			mapia_write_rbuf(&r->vm_start, sizeof(r->vm_start));
			mapia_write_rbuf(&r->vm_end, sizeof(r->vm_end));
			mapia_write_rbuf(&r->nr_accesses,
					sizeof(r->nr_accesses));
			r->nr_accesses = 0;
		}
	}
}

/*
 * Check whether regions need to be updated
 *
 * Returns 1 if it is, 0 else.
 */
static int need_update_regions(void)
{
	return mapia_check_time_interval_reset(&last_regions_update_time,
			regions_update_interval);
}

static bool mapia_intersect(struct mapia_region *r, struct region *re)
{
	if (r->vm_end <= re->start || r->vm_start >= re->end)
		return false;
	return true;
}

static bool mapia_intersect_three(struct mapia_region *r,
		struct region regions[3])
{
	int i;

	if (r->vm_end <= regions[0].start || regions[2].end <= r->vm_start)
		return false;

	for (i = 0; i < 2; i++) {
		if (regions[i].end <= r->vm_start &&
				r->vm_end <= regions[i + 1].start)
			return false;
	}
	return true;
}

static void mapia_update_two_gaps(struct mapia_task *t,
				struct region regions[3])
{
	struct mapia_region *r, *next;
	unsigned int i = 0;

	mapia_for_each_region_safe(r, next, t) {
		if (!mapia_intersect_three(r, regions)) {
			mapia_rm_region(r);
			mapia_destroy_region(r);
		}
	}

	for (i = 0; i < 3; i++) {
		struct mapia_region *first = NULL, *last;
		struct mapia_region *newr;
		struct region *re;

		re = &regions[i];
		mapia_for_each_region(r, t) {
			if (mapia_intersect(r, re)) {
				if (!first)
					first = r;
				last = r;
			}
			if (r->vm_start >= re->end)
				break;
		}
		if (!first) {
			newr = mapia_new_region(re->start, re->end);
			__mapia_add_region(newr, mapia_prev_region(r), r);
		} else {
			first->vm_start = re->start;
			last->vm_end = re->end;
		}
	}
}

/*
 * Update regions with merging / splitting if necessary
 */
static void update_regions(void)
{
	struct region three_regions[3];
	struct mapia_task *t;

	mapia_for_each_task(t) {
		mapia_three_regions_of(t, three_regions);
		mapia_update_two_gaps(t, three_regions);
	}
}

/*
 * Check whether tracking loop should be stopped
 *
 * Returns true if it is, false else.
 */
static bool need_stop(void)
{
	struct mapia_task *t;
	struct task_struct *task;
	bool all_died = true;

	rcu_read_lock();
	mapia_for_each_task(t) {
		task = pid_task(find_vpid(t->pid), PIDTYPE_PID);
		if (task != NULL) {
			all_died = false;
			break;
		}
	}
	rcu_read_unlock();

	if (all_died)
		return true;

	return mapia_tracing_on == false;
}

static int mapia_trace_thread(void *data)
{
	struct mapia_task *t, *t_next;
	struct mapia_region *r;

	pr_info("track\n");

	init_regions();
	while (!need_stop()) {
		mapia_for_each_task(t) {
			mapia_for_each_region(r, t)
				check_access(t, r);
		}

		/*
		 * This is the core logic for the tradeoff of accuracy and
		 * efficiency.
		 *
		 * Merging minimizes number of regions by merging adjacent
		 * regions that has similar access frequencies.
		 * Aggregation (maybe recording would be better name) becomes
		 * efficient because it need to record only the regions after
		 * merging.
		 *
		 * Splitting improves accuracy by increasing sampling overhead
		 * twice.  That said, the overhead will be bounded because the
		 * number of regions cannot exceed the 'max_nr_regions' after
		 * the splitting.  It splits each region into randomly sized
		 * two new regions.  Because of the random property, some
		 * splittings will be judged as meaningless.  Later merging
		 * will judge that and restore the splitted regions into
		 * original form.  Meanwhile, meaningful splitting will leave
		 * the effect.
		 *
		 * TODO: Adaptive adjustment of the 'mapia_merge_threshold'.
		 * It is essential for better accuracy and efficient tradeoff.
		 */
		if (need_aggregate()) {
			mapia_merge_regions();
			aggregate();
			if (need_update_regions())
				update_regions();
			mapia_split_regions();
		}

		udelay(sample_interval / 1000);
	}
	pr_info("final flush\n");
	mapia_flush_rbuffer();
	mapia_for_each_task_safe(t, t_next) {
		mapia_rm_task(t);
		mapia_destroy_task(t);
	}
	mapia_trace_task = NULL;
	return 0;
}

static long mapia_turn_trace(bool on)
{
	mapia_tracing_on = on;

	if (mapia_trace_task == NULL && mapia_tracing_on) {
		mapia_trace_task = kthread_run(mapia_trace_thread, NULL,
				"mapia_tracer");
		if (!mapia_trace_task) {
			pr_err("failed to create mapia_tracer\n");
			return -ENOMEM;
		}
		pr_info("mapia_tracer (%d) started from %d\n",
				mapia_trace_task->pid, current->pid);
	}

	return 0;
}

#define LEN_DBG_PIDS_BUF 1024
static char dbg_pids_buf[LEN_DBG_PIDS_BUF];

static ssize_t mapia_pids_str(char *buf, ssize_t len)
{
	char *cursor = buf;
	unsigned long i;
	struct mapia_task *t;

	for (i = 0; i < len; i++)
		cursor[i] = '\0';
	mapia_for_each_task(t) {
		snprintf(cursor, len, "%lu ", t->pid);
		cursor += strnlen(cursor, len);
	}
	if (strnlen(buf, len) > 0)
		cursor[strnlen(cursor, len) - 1] = '\n';
	else
		buf[0] = '\n';
	return strnlen(buf, len);
}

static ssize_t debugfs_pids_read(struct file *file,
		char __user *buf, size_t count, loff_t *ppos)
{
	mapia_pids_str(dbg_pids_buf, LEN_DBG_PIDS_BUF);

	return simple_read_from_buffer(buf, count, ppos, dbg_pids_buf,
			LEN_DBG_PIDS_BUF);
}

static ssize_t nr_chrs(const char *str, ssize_t len_str, const char chr)
{
	ssize_t ret, i;

	ret = 0;
	for (i = 0; i < len_str; i++) {
		if (str[i] == '\0')
			break;
		if (str[i] == chr)
			ret++;
	}
	return ret;
}

static const char *next_chr(const char *str, ssize_t len, const char chr)
{
	ssize_t i;

	for (i = 0; i < len; i++) {
		if (str[0] == '\0')
			break;
		if (str[0] == chr)
			break;
		str++;
	}

	return str;
}

/*
 * Converts a string into an array of unsigned long integers
 *
 * str		string to be converted
 * len		length of the string
 * nr_integers	Pointer that size of the converted array will be stored
 *
 * This function converts a string into an array of unsigned long integers and
 * saves the number of integers in the array.  The input string should be in
 * format of integers seperated by a single space.
 * Also, please note that the returned array should be freed by caller.
 *
 * Returns an array of unsigned long integers that converted.
 */
static unsigned long *str_to_ints(const char *str, ssize_t len,
		ssize_t *nr_integers)
{
	unsigned long *list;
	ssize_t i;

	if (str == NULL || len == 0 || (str[0] < '0' || str[0] > '9')) {
		*nr_integers = 0;
		return NULL;
	}
	*nr_integers = nr_chrs(str, len, ' ') + 1;
	list = kmalloc_array(*nr_integers, sizeof(unsigned long), GFP_KERNEL);

	for (i = 0; i < *nr_integers; i++) {
		if (sscanf(str, "%lu ", &list[i]) != 1)
			break;
		str = next_chr(str, len, ' ') + 1;
	}

	return list;
}

static ssize_t debugfs_pids_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	ssize_t ret;
	unsigned long *targets;
	ssize_t nr_targets;

	ret = simple_write_to_buffer(dbg_pids_buf, LEN_DBG_PIDS_BUF,
			ppos, buf, count);
	if (ret < 0)
		return ret;

	targets = str_to_ints(dbg_pids_buf, ret, &nr_targets);
	mapia_set_pids(targets, nr_targets);
	kfree(targets);

	return ret;
}

#define LEN_DBG_ATTRS_BUF 64
static char dbg_attrs_buf[LEN_DBG_ATTRS_BUF];

static ssize_t debugfs_attrs_read(struct file *file,
		char __user *buf, size_t count, loff_t *ppos)
{
	unsigned long i;

	for (i = 0; i < LEN_DBG_ATTRS_BUF; i++)
		dbg_attrs_buf[i] = '\0';
	snprintf(dbg_attrs_buf, LEN_DBG_ATTRS_BUF, "%lu %lu %lu %lu %lu %s\n",
			sample_interval, aggr_interval,
			regions_update_interval, min_nr_regions,
			max_nr_regions, rfile_path);

	return simple_read_from_buffer(buf, count, ppos, dbg_attrs_buf,
			LEN_DBG_ATTRS_BUF);
}

static ssize_t debugfs_attrs_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	unsigned long s, a, r, minr, maxr;
	char res_file_path[LEN_RES_FILE_PATH];
	ssize_t ret;

	ret = simple_write_to_buffer(dbg_attrs_buf, LEN_DBG_ATTRS_BUF,
			ppos, buf, count);
	if (ret < 0)
		return ret;

	if (sscanf(dbg_attrs_buf, "%lu %lu %lu %lu %lu %s",
				&s, &a, &r, &minr, &maxr, res_file_path) != 6)
		return -EINVAL;

	mapia_set_attrs(s, a, r, minr, maxr, res_file_path);

	return ret;
}

#define LEN_DBG_TRACING_ON_BUF 64
static char dbg_tracing_on_buf[LEN_DBG_TRACING_ON_BUF];

static ssize_t debugfs_tracing_on_read(struct file *file,
		char __user *buf, size_t count, loff_t *ppos)
{
	unsigned long i;

	for (i = 0; i < LEN_DBG_TRACING_ON_BUF; i++)
		dbg_tracing_on_buf[i] = '\0';
	snprintf(dbg_tracing_on_buf, LEN_DBG_TRACING_ON_BUF,
			mapia_tracing_on ? "on\n" : "off\n");

	return simple_read_from_buffer(buf, count, ppos, dbg_tracing_on_buf,
			LEN_DBG_TRACING_ON_BUF);
}

static ssize_t debugfs_tracing_on_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	ssize_t ret;
	bool on = false;
	char cmdbuf[LEN_DBG_TRACING_ON_BUF];

	ret = simple_write_to_buffer(dbg_tracing_on_buf, LEN_DBG_TRACING_ON_BUF,
			ppos, buf, count);
	if (ret < 0)
		return ret;

	if (sscanf(dbg_tracing_on_buf, "%s", cmdbuf) != 1)
		return -EINVAL;
	if (!strncmp(cmdbuf, "on", LEN_DBG_TRACING_ON_BUF))
		on = true;
	else if (!strncmp(cmdbuf, "off", LEN_DBG_TRACING_ON_BUF))
		on = false;
	else
		return -EINVAL;
	mapia_turn_trace(on);

	return ret;
}

static const struct file_operations pids_fops = {
	.owner = THIS_MODULE,
	.read = debugfs_pids_read,
	.write = debugfs_pids_write,
};

static const struct file_operations attrs_fops = {
	.owner = THIS_MODULE,
	.read = debugfs_attrs_read,
	.write = debugfs_attrs_write,
};

static const struct file_operations tracing_on_fops = {
	.owner = THIS_MODULE,
	.read = debugfs_tracing_on_read,
	.write = debugfs_tracing_on_write,
};

static struct dentry *debugfs_root;

static int __init debugfs_init(void)
{
	debugfs_root = debugfs_create_dir("mapia", NULL);
	if (!debugfs_root) {
		pr_err("failed to create debugfs\n");
		return -ENOMEM;
	}

	if (!debugfs_create_file("pids", 0600, debugfs_root, NULL,
				&pids_fops)) {
		pr_err("failed to create pids file\n");
		return -ENOMEM;
	}

	if (!debugfs_create_file("attrs", 0600, debugfs_root, NULL,
				&attrs_fops)) {
		pr_err("failed to create attrs file\n");
		return -ENOMEM;
	}

	if (!debugfs_create_file("tracing_on", 0600, debugfs_root, NULL,
				&tracing_on_fops)) {
		pr_err("failed to create attrs file\n");
		return -ENOMEM;
	}

	return 0;
}

static void __exit debugfs_exit(void)
{
	debugfs_remove_recursive(debugfs_root);
}

static int __init mapia_init(void)
{
	pr_info("init\n");

	prandom_seed_state(&rndseed, 42);
	ktime_get_coarse_ts64(&last_aggregate_time);
	last_regions_update_time = last_aggregate_time;

	debugfs_init();

	return 0;
}

static void __exit mapia_exit(void)
{
	pr_info("exit\n");

	mapia_tracing_on = false;
	while (mapia_trace_task)
		msleep(100);

	debugfs_exit();
}

module_init(mapia_init);
module_exit(mapia_exit);

MODULE_LICENSE("GPL");
