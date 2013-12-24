/*
 * Author: Alucard_24@XDA
 *
 * Copyright 2012 Alucard_24@XDA
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>
#ifndef CONFIG_CPU_EXYNOS4210
#include "acpuclock.h"
#endif

static DEFINE_MUTEX(alucard_hotplug_mutex);
static struct mutex timer_mutex;

static struct delayed_work alucard_hotplug_work;
static struct work_struct alucard_hotplug_offline_work;
static struct work_struct alucard_hotplug_online_work;

struct hotplug_cpuinfo {
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_idle;
	int online;
	int up_cpu;
	int up_by_cpu;
};

static DEFINE_PER_CPU(struct hotplug_cpuinfo, od_hotplug_cpuinfo);

static struct hotplug_tuners {
	atomic_t hotplug_sampling_rate;
	atomic_t hotplug_enable;
	atomic_t cpu_up_rate;
	atomic_t cpu_down_rate;
	atomic_t maxcoreslimit;
#ifndef CONFIG_CPU_EXYNOS4210
	atomic_t accuratecpufreq;
#endif
} hotplug_tuners_ins = {
	.hotplug_sampling_rate = ATOMIC_INIT(60000),
	.hotplug_enable = ATOMIC_INIT(0),
	.cpu_up_rate = ATOMIC_INIT(10),
	.cpu_down_rate = ATOMIC_INIT(20),
	.maxcoreslimit = ATOMIC_INIT(NR_CPUS),
#ifndef CONFIG_CPU_EXYNOS4210
	.accuratecpufreq = ATOMIC_INIT(0),
#endif
};

#define MAX_HOTPLUG_RATE		(40)
#define DOWN_INDEX		(0)
#define UP_INDEX		(1)

#ifndef CONFIG_CPU_EXYNOS4210
#define RQ_AVG_TIMER_RATE	10
#else
#define RQ_AVG_TIMER_RATE	20
#endif

struct runqueue_data {
	unsigned int nr_run_avg;
	unsigned int update_rate;
	int64_t last_time;
	int64_t total_time;
	struct delayed_work work;
	struct workqueue_struct *nr_run_wq;
	spinlock_t lock;
};

static struct runqueue_data *rq_data;
static void rq_work_fn(struct work_struct *work);

static void start_rq_work(void)
{
	rq_data->nr_run_avg = 0;
	rq_data->last_time = 0;
	rq_data->total_time = 0;
	if (rq_data->nr_run_wq == NULL)
		rq_data->nr_run_wq =
			create_singlethread_workqueue("nr_run_avg");

	queue_delayed_work(rq_data->nr_run_wq, &rq_data->work,
			   msecs_to_jiffies(rq_data->update_rate));
	return;
}

static void stop_rq_work(void)
{
	if (rq_data->nr_run_wq)
		cancel_delayed_work(&rq_data->work);
	return;
}

static int __init init_rq_avg(void)
{
	rq_data = kzalloc(sizeof(struct runqueue_data), GFP_KERNEL);
	if (rq_data == NULL) {
		pr_err("%s cannot allocate memory\n", __func__);
		return -ENOMEM;
	}
	spin_lock_init(&rq_data->lock);
	rq_data->update_rate = RQ_AVG_TIMER_RATE;
	INIT_DELAYED_WORK_DEFERRABLE(&rq_data->work, rq_work_fn);

	return 0;
}

static void rq_work_fn(struct work_struct *work)
{
	int64_t time_diff = 0;
	int64_t nr_run = 0;
	unsigned long flags = 0;
	int64_t cur_time;

	cur_time = ktime_to_ns(ktime_get());

	spin_lock_irqsave(&rq_data->lock, flags);

	if (rq_data->last_time == 0)
		rq_data->last_time = cur_time;
	if (rq_data->nr_run_avg == 0)
		rq_data->total_time = 0;

	nr_run = nr_running() * 100;
	time_diff = cur_time - rq_data->last_time;
	do_div(time_diff, 1000 * 1000);

	if (time_diff != 0 && rq_data->total_time != 0) {
		nr_run = (nr_run * time_diff) +
			(rq_data->nr_run_avg * rq_data->total_time);
		do_div(nr_run, rq_data->total_time + time_diff);
	}
	rq_data->nr_run_avg = nr_run;
	rq_data->total_time += time_diff;
	rq_data->last_time = cur_time;

	if (rq_data->update_rate != 0)
		queue_delayed_work(rq_data->nr_run_wq, &rq_data->work,
				   msecs_to_jiffies(rq_data->update_rate));

	spin_unlock_irqrestore(&rq_data->lock, flags);
}

static unsigned int get_nr_run_avg(void)
{
	unsigned int nr_run_avg;
	unsigned long flags = 0;

	spin_lock_irqsave(&rq_data->lock, flags);
	nr_run_avg = rq_data->nr_run_avg;
	rq_data->nr_run_avg = 0;
	spin_unlock_irqrestore(&rq_data->lock, flags);

	return nr_run_avg;
}

static unsigned hotplugging_rate = 0;
static bool other_hotplugging = false;

#ifdef CONFIG_CPU_EXYNOS4210
static atomic_t hotplug_freq[2][2] = {
	{ATOMIC_INIT(0), ATOMIC_INIT(800000)},
	{ATOMIC_INIT(500000), ATOMIC_INIT(0)}
};
static atomic_t hotplug_load[2][2] = {
	{ATOMIC_INIT(0), ATOMIC_INIT(65)},
	{ATOMIC_INIT(30), ATOMIC_INIT(0)}
};
static atomic_t hotplug_rq[2][2] = {
	{ATOMIC_INIT(0), ATOMIC_INIT(200)}, 
	{ATOMIC_INIT(300), ATOMIC_INIT(0)}
};
#else
static atomic_t hotplug_freq[4][2] = {
	{ATOMIC_INIT(0), ATOMIC_INIT(702000)},
	{ATOMIC_INIT(486000), ATOMIC_INIT(702000)},
	{ATOMIC_INIT(486000), ATOMIC_INIT(702000)},
	{ATOMIC_INIT(486000), ATOMIC_INIT(0)}
};
static atomic_t hotplug_load[4][2] = {
	{ATOMIC_INIT(0), ATOMIC_INIT(65)},
	{ATOMIC_INIT(30), ATOMIC_INIT(65)},
	{ATOMIC_INIT(30), ATOMIC_INIT(65)},
	{ATOMIC_INIT(30), ATOMIC_INIT(0)}
};
static atomic_t hotplug_rq[4][2] = {
	{ATOMIC_INIT(0), ATOMIC_INIT(200)}, 
	{ATOMIC_INIT(200), ATOMIC_INIT(200)}, 
	{ATOMIC_INIT(200), ATOMIC_INIT(300)}, 
	{ATOMIC_INIT(300), ATOMIC_INIT(0)}
};
#endif

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%d\n", atomic_read(&hotplug_tuners_ins.object));		\
}
show_one(hotplug_sampling_rate, hotplug_sampling_rate);
show_one(hotplug_enable, hotplug_enable);
show_one(cpu_up_rate, cpu_up_rate);
show_one(cpu_down_rate, cpu_down_rate);
show_one(maxcoreslimit, maxcoreslimit);
#ifndef CONFIG_CPU_EXYNOS4210
show_one(accuratecpufreq, accuratecpufreq);
#endif

#define show_hotplug_param(file_name, num_core, up_down)		\
static ssize_t show_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%d\n", atomic_read(&file_name[num_core - 1][up_down]));	\
}

#define store_hotplug_param(file_name, num_core, up_down)		\
static ssize_t store_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr,				\
	const char *buf, size_t count)					\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%d", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	if (input == atomic_read(&file_name[num_core - 1][up_down])) {		\
		return count;	\
	}	\
	atomic_set(&file_name[num_core - 1][up_down], input);			\
	return count;							\
}

/* hotplug freq */
show_hotplug_param(hotplug_freq, 1, 1);
show_hotplug_param(hotplug_freq, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_freq, 2, 1);
show_hotplug_param(hotplug_freq, 3, 0);
show_hotplug_param(hotplug_freq, 3, 1);
show_hotplug_param(hotplug_freq, 4, 0);
#endif
/* hotplug load */
show_hotplug_param(hotplug_load, 1, 1);
show_hotplug_param(hotplug_load, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_load, 2, 1);
show_hotplug_param(hotplug_load, 3, 0);
show_hotplug_param(hotplug_load, 3, 1);
show_hotplug_param(hotplug_load, 4, 0);
#endif
/* hotplug rq */
show_hotplug_param(hotplug_rq, 1, 1);
show_hotplug_param(hotplug_rq, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_rq, 2, 1);
show_hotplug_param(hotplug_rq, 3, 0);
show_hotplug_param(hotplug_rq, 3, 1);
show_hotplug_param(hotplug_rq, 4, 0);
#endif

/* hotplug freq */
store_hotplug_param(hotplug_freq, 1, 1);
store_hotplug_param(hotplug_freq, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_freq, 2, 1);
store_hotplug_param(hotplug_freq, 3, 0);
store_hotplug_param(hotplug_freq, 3, 1);
store_hotplug_param(hotplug_freq, 4, 0);
#endif
/* hotplug load */
store_hotplug_param(hotplug_load, 1, 1);
store_hotplug_param(hotplug_load, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_load, 2, 1);
store_hotplug_param(hotplug_load, 3, 0);
store_hotplug_param(hotplug_load, 3, 1);
store_hotplug_param(hotplug_load, 4, 0);
#endif
/* hotplug rq */
store_hotplug_param(hotplug_rq, 1, 1);
store_hotplug_param(hotplug_rq, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_rq, 2, 1);
store_hotplug_param(hotplug_rq, 3, 0);
store_hotplug_param(hotplug_rq, 3, 1);
store_hotplug_param(hotplug_rq, 4, 0);
#endif

define_one_global_rw(hotplug_freq_1_1);
define_one_global_rw(hotplug_freq_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_freq_2_1);
define_one_global_rw(hotplug_freq_3_0);
define_one_global_rw(hotplug_freq_3_1);
define_one_global_rw(hotplug_freq_4_0);
#endif

define_one_global_rw(hotplug_load_1_1);
define_one_global_rw(hotplug_load_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_load_2_1);
define_one_global_rw(hotplug_load_3_0);
define_one_global_rw(hotplug_load_3_1);
define_one_global_rw(hotplug_load_4_0);
#endif

define_one_global_rw(hotplug_rq_1_1);
define_one_global_rw(hotplug_rq_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_rq_2_1);
define_one_global_rw(hotplug_rq_3_0);
define_one_global_rw(hotplug_rq_3_1);
define_one_global_rw(hotplug_rq_4_0);
#endif

static void __ref cpus_hotplugging(bool state) {
	unsigned int cpu=0;
	int delay = 0;

	mutex_lock(&timer_mutex);

	if (state) {
		start_rq_work();
		for_each_possible_cpu(cpu) {
			per_cpu(od_hotplug_cpuinfo, cpu).prev_cpu_idle = get_cpu_idle_time_us(cpu, NULL);
			per_cpu(od_hotplug_cpuinfo, cpu).prev_cpu_idle += get_cpu_iowait_time_us(cpu, &per_cpu(od_hotplug_cpuinfo, cpu).prev_cpu_wall);
			per_cpu(od_hotplug_cpuinfo, cpu).up_cpu = 1;
			per_cpu(od_hotplug_cpuinfo, cpu).online = cpu_online(cpu);
			per_cpu(od_hotplug_cpuinfo, cpu).up_by_cpu = -1;
		}
		hotplugging_rate = 0;
		delay = usecs_to_jiffies(atomic_read(&hotplug_tuners_ins.hotplug_sampling_rate));
		/*if (num_online_cpus() > 1) {
			delay -= jiffies % delay;
		}*/
		schedule_delayed_work(&alucard_hotplug_work, delay);
	} else {
		stop_rq_work();
		for_each_online_cpu(cpu) {
			if (cpu == 0)
				continue;
			cpu_down(cpu);
		}
	}

	mutex_unlock(&timer_mutex);
}

/**
 * update_sampling_rate - update sampling rate effective immediately if needed.
 * @new_rate: new sampling rate
 *
 * If new rate is smaller than the old, simply updaing
 * hotplug_tuners_ins.hotplug_sampling_rate might not be appropriate. For example,
 * if the original sampling_rate was 1 second and the requested new sampling
 * rate is 10 ms because the user needs immediate reaction from ondemand
 * governor, but not sure if higher frequency will be required or not,
 * then, the hotplugging system may change the sampling rate too late; up to 1 second
 * later. Thus, if we are reducing the hotplug sampling rate, we need to make the
 * new value effective immediately.
 */
static void update_sampling_rate(unsigned int new_rate)
{
	int cpu=0;
	unsigned long next_sampling, appointed_at;

	atomic_set(&hotplug_tuners_ins.hotplug_sampling_rate,new_rate);

	mutex_lock(&timer_mutex);

	if (!delayed_work_pending(&alucard_hotplug_work)) {
		mutex_unlock(&timer_mutex);
		return;
	}

	next_sampling  = jiffies + usecs_to_jiffies(new_rate);
	appointed_at = alucard_hotplug_work.timer.expires;

	if (time_before(next_sampling, appointed_at)) {

		mutex_unlock(&timer_mutex);
		cancel_delayed_work_sync(&alucard_hotplug_work);
		mutex_lock(&timer_mutex);

		schedule_delayed_work_on(cpu, &alucard_hotplug_work, usecs_to_jiffies(new_rate));
	}

	mutex_unlock(&timer_mutex);
}

/* hotplug_sampling_rate */
static ssize_t store_hotplug_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input,10000);
	
	if (input == atomic_read(&hotplug_tuners_ins.hotplug_sampling_rate))
		return count;

	update_sampling_rate(input);

	return count;
}

/* hotplug_enable */
static ssize_t store_hotplug_enable(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = input > 0; 

	if (atomic_read(&hotplug_tuners_ins.hotplug_enable) == input)
		return count;

	if (input > 0)
		cpus_hotplugging(true);
	else
		cpus_hotplugging(false);

	atomic_set(&hotplug_tuners_ins.hotplug_enable, input);

	return count;
}

/* cpu_up_rate */
static ssize_t store_cpu_up_rate(struct kobject *a, struct attribute *b,
				 const char *buf, size_t count)
{
	int input;
	int ret;
	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,MAX_HOTPLUG_RATE),1);

	if (input == atomic_read(&hotplug_tuners_ins.cpu_up_rate))
		return count;

	atomic_set(&hotplug_tuners_ins.cpu_up_rate,input);

	return count;
}

/* cpu_down_rate */
static ssize_t store_cpu_down_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,MAX_HOTPLUG_RATE),1);

	if (input == atomic_read(&hotplug_tuners_ins.cpu_down_rate))
		return count;

	atomic_set(&hotplug_tuners_ins.cpu_down_rate,input);
	return count;
}

/* maxcoreslimit */
static ssize_t store_maxcoreslimit(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input > NR_CPUS ? NR_CPUS : input, 1); 

	if (atomic_read(&hotplug_tuners_ins.maxcoreslimit) == input)
		return count;

	atomic_set(&hotplug_tuners_ins.maxcoreslimit, input);

	return count;
}

#ifndef CONFIG_CPU_EXYNOS4210
/* accuratecpufreq */
static ssize_t store_accuratecpufreq(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = input > 0; 

	if (atomic_read(&hotplug_tuners_ins.accuratecpufreq) == input)
		return count;

	atomic_set(&hotplug_tuners_ins.accuratecpufreq, input);

	return count;
}
#endif
define_one_global_rw(hotplug_sampling_rate);
define_one_global_rw(hotplug_enable);
define_one_global_rw(cpu_up_rate);
define_one_global_rw(cpu_down_rate);
define_one_global_rw(maxcoreslimit);
#ifndef CONFIG_CPU_EXYNOS4210
define_one_global_rw(accuratecpufreq);
#endif

static struct attribute *alucard_hotplug_attributes[] = {
	&hotplug_sampling_rate.attr,
	&hotplug_enable.attr,
	&hotplug_freq_1_1.attr,
	&hotplug_freq_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_freq_2_1.attr,
	&hotplug_freq_3_0.attr,
	&hotplug_freq_3_1.attr,
	&hotplug_freq_4_0.attr,
#endif
	&hotplug_load_1_1.attr,
	&hotplug_load_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_load_2_1.attr,
	&hotplug_load_3_0.attr,
	&hotplug_load_3_1.attr,
	&hotplug_load_4_0.attr,
#endif
	&hotplug_rq_1_1.attr,
	&hotplug_rq_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_rq_2_1.attr,
	&hotplug_rq_3_0.attr,
	&hotplug_rq_3_1.attr,
	&hotplug_rq_4_0.attr,
#endif
	&cpu_up_rate.attr,
	&cpu_down_rate.attr,
	&maxcoreslimit.attr,
#ifndef CONFIG_CPU_EXYNOS4210
	&accuratecpufreq.attr,
#endif
	NULL
};

static struct attribute_group alucard_hotplug_attr_group = {
	.attrs = alucard_hotplug_attributes,
	.name = "alucard_hotplug",
};

static void __cpuinit cpu_online_work_fn(struct work_struct *work)
{
	int cpu;
	for_each_cpu_not(cpu, cpu_online_mask) {
		if (per_cpu(od_hotplug_cpuinfo, cpu).online == true) {
			cpu_up(cpu);
		}
	}
}

static void __ref cpu_offline_work_fn(struct work_struct *work)
{
	int cpu;
	for_each_online_cpu(cpu) {
		if (per_cpu(od_hotplug_cpuinfo, cpu).online == false) {
			cpu_down(cpu);
		}
	}
	if (num_online_cpus() == 1) {
		per_cpu(od_hotplug_cpuinfo, 0).up_cpu = 1;
	}
}

static void hotplug_work_fn(struct work_struct *work)
{
	bool hotplug_enable = atomic_read(&hotplug_tuners_ins.hotplug_enable) > 0;
	int upmaxcoreslimit = atomic_read(&hotplug_tuners_ins.maxcoreslimit);
	int downmaxcoreslimit = (upmaxcoreslimit == NR_CPUS ? 0 : upmaxcoreslimit - 1);
	int up_rate = atomic_read(&hotplug_tuners_ins.cpu_up_rate);
	int down_rate = atomic_read(&hotplug_tuners_ins.cpu_down_rate);
#ifndef CONFIG_CPU_EXYNOS4210
	bool accuratecpufreq = atomic_read(&hotplug_tuners_ins.accuratecpufreq)  > 0;
#endif
	bool check_up = false, check_down = false;
	int schedule_down_cpu = 1;
	int schedule_up_cpu = 1;
	unsigned int cpu = 0;
	unsigned int rq_avg = 0;
	int up_by_cpu = -1;
	int up_cpu_req = -1;
	int delay;

	mutex_lock(&timer_mutex);

	if (hotplug_enable) {
		/* set hotplugging_rate used */
		++hotplugging_rate;
 		check_up = (hotplugging_rate % up_rate == 0);
		check_down = (hotplugging_rate % down_rate == 0);
		other_hotplugging = false;
		rq_avg = get_nr_run_avg();

		for_each_possible_cpu(cpu) {
			cputime64_t cur_wall_time, cur_idle_time;
			unsigned int wall_time, idle_time;
			int up_load;
			int down_load;
			unsigned int up_freq;
			unsigned int down_freq;
			unsigned int up_rq;
			unsigned int down_rq;
			int online;
			int cur_load = -1;
			unsigned int cur_freq = 0;

			cur_idle_time = get_cpu_idle_time_us(cpu, NULL);
			cur_idle_time += get_cpu_iowait_time_us(cpu, &cur_wall_time);

			wall_time = (unsigned int)
					(cur_wall_time - per_cpu(od_hotplug_cpuinfo, cpu).prev_cpu_wall);
			per_cpu(od_hotplug_cpuinfo, cpu).prev_cpu_wall = cur_wall_time;

			idle_time = (unsigned int)
					(cur_idle_time - per_cpu(od_hotplug_cpuinfo, cpu).prev_cpu_idle);
			per_cpu(od_hotplug_cpuinfo, cpu).prev_cpu_idle = cur_idle_time;

			up_load = atomic_read(&hotplug_load[cpu][UP_INDEX]);
			down_load = atomic_read(&hotplug_load[cpu][DOWN_INDEX]);
			up_freq = atomic_read(&hotplug_freq[cpu][UP_INDEX]);
			down_freq = atomic_read(&hotplug_freq[cpu][DOWN_INDEX]);
			up_rq = atomic_read(&hotplug_rq[cpu][UP_INDEX]);
			down_rq = atomic_read(&hotplug_rq[cpu][DOWN_INDEX]);

			online = cpu_online(cpu);
			if (per_cpu(od_hotplug_cpuinfo, cpu).online != online) {
				other_hotplugging = true;
			} else if (!online) {
				per_cpu(od_hotplug_cpuinfo, cpu).up_cpu = 1;
				up_by_cpu = per_cpu(od_hotplug_cpuinfo, cpu).up_by_cpu;
				if (up_by_cpu >= 0) {
					per_cpu(od_hotplug_cpuinfo, up_by_cpu).up_cpu = 1;
					per_cpu(od_hotplug_cpuinfo, cpu).up_by_cpu = -1;
				}
			}
			if (other_hotplugging == false) {
				/*printk(KERN_ERR "TIMER CPU[%u], wall[%u], idle[%u]\n",j, wall_time, idle_time);*/
				if (wall_time >= idle_time && online) { /*if wall_time < idle_time, evaluate cpu load next time*/
					cur_load = wall_time > idle_time ? (100 * (wall_time - idle_time)) / wall_time : 0;/*if wall_time is equal to idle_time cpu_load is equal to 0*/
#ifndef CONFIG_CPU_EXYNOS4210
					if (accuratecpufreq)
						cur_freq = acpuclk_get_rate(cpu);
					else
						cur_freq = cpufreq_quick_get(cpu);
#else
					cur_freq = cpufreq_quick_get(cpu);
#endif
				} else {
					cur_load = -1;
					cur_freq = 0;
				}
				if (check_up 
					&& cpu < upmaxcoreslimit - 1 
					&& per_cpu(od_hotplug_cpuinfo, cpu).up_cpu > 0
					&& schedule_up_cpu > 0
					&& online) {
						if (cur_load >= up_load
							&& cur_freq >= up_freq
							&& rq_avg > up_rq) {
							schedule_up_cpu--;
							per_cpu(od_hotplug_cpuinfo, cpu).up_cpu = 0;
							up_cpu_req = cpu;
						}
				}
				if (check_down
					&& cpu > downmaxcoreslimit
					&& online
					&& schedule_down_cpu > 0
					&& cur_load >= 0) {
						if ((online >=2
							&& cur_load < down_load)
							|| (cur_freq <= down_freq
								&& rq_avg <= down_rq)) {
								per_cpu(od_hotplug_cpuinfo, cpu).online = false;
								schedule_down_cpu--;
								schedule_work(&alucard_hotplug_offline_work);
						}
				}
				if (schedule_up_cpu == 0 && !online) {
					per_cpu(od_hotplug_cpuinfo, cpu).online = true;
					per_cpu(od_hotplug_cpuinfo, cpu).up_by_cpu = up_cpu_req;
					schedule_up_cpu--;
					schedule_work(&alucard_hotplug_online_work);
				}
			}
		}

		if (other_hotplugging == true) {
			for_each_possible_cpu(cpu) {
				per_cpu(od_hotplug_cpuinfo, cpu).online = cpu_online(cpu);
				per_cpu(od_hotplug_cpuinfo, cpu).up_cpu = 1;
				per_cpu(od_hotplug_cpuinfo, cpu).up_by_cpu = -1;
			}
			other_hotplugging = false;
		}

		if (hotplugging_rate >= max(up_rate, down_rate)) {
			hotplugging_rate = 0;
		}

		delay = usecs_to_jiffies(atomic_read(&hotplug_tuners_ins.hotplug_sampling_rate));
		/*if (num_online_cpus() > 1) {
			delay -= jiffies % delay;
		} else {*/
		if (num_online_cpus() == 1) {
			per_cpu(od_hotplug_cpuinfo, 0).up_cpu = 1;
		}
		schedule_delayed_work(&alucard_hotplug_work, delay);
	}
	mutex_unlock(&timer_mutex);
}

int __init alucard_hotplug_init(void)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay;
	unsigned int cpu;
	int ret;

	ret = sysfs_create_group(cpufreq_global_kobject, &alucard_hotplug_attr_group);
	if (ret) {
		printk(KERN_ERR "failed at(%d)\n", __LINE__);
		return ret;
	}

	ret = init_rq_avg();
	if (ret) {
		return ret;
	}

	if (atomic_read(&hotplug_tuners_ins.hotplug_enable) > 0) {
		start_rq_work();
	}

	mutex_lock(&alucard_hotplug_mutex);
	hotplugging_rate = 0;
	for_each_possible_cpu(cpu) {
		per_cpu(od_hotplug_cpuinfo, cpu).prev_cpu_idle = get_cpu_idle_time_us(cpu, NULL);
		per_cpu(od_hotplug_cpuinfo, cpu).prev_cpu_idle += get_cpu_iowait_time_us(cpu, &per_cpu(od_hotplug_cpuinfo, cpu).prev_cpu_wall);
		
		per_cpu(od_hotplug_cpuinfo, cpu).up_cpu = 1;
		per_cpu(od_hotplug_cpuinfo, cpu).online = cpu_online(cpu);
		per_cpu(od_hotplug_cpuinfo, cpu).up_by_cpu = -1;
	}
	mutex_init(&timer_mutex);
	mutex_unlock(&alucard_hotplug_mutex);

	delay = usecs_to_jiffies(atomic_read(&hotplug_tuners_ins.hotplug_sampling_rate));
	/*if (num_online_cpus() > 1) {
		delay -= jiffies % delay;
	}*/
	INIT_DELAYED_WORK(&alucard_hotplug_work, hotplug_work_fn);
	INIT_WORK(&alucard_hotplug_online_work, cpu_online_work_fn);
	INIT_WORK(&alucard_hotplug_offline_work, cpu_offline_work_fn);
	schedule_delayed_work(&alucard_hotplug_work, delay);

	return ret;
}

static void __exit alucard_hotplug_exit(void)
{
	cancel_delayed_work_sync(&alucard_hotplug_work);
	cancel_work_sync(&alucard_hotplug_online_work);
	cancel_work_sync(&alucard_hotplug_offline_work);
	mutex_destroy(&timer_mutex);
}
MODULE_AUTHOR("Alucard_24@XDA");
MODULE_DESCRIPTION("'alucard_hotplug' - A cpu hotplug driver for "
	"capable processors");
MODULE_LICENSE("GPL");
module_init(alucard_hotplug_init);
module_exit(alucard_hotplug_exit);
