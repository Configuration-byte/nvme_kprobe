// io_delay_kprobe.c - 通过 kprobe 拦截 nvme_complete_rq 实现 IO 延迟
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/hrtimer.h>
#include <linux/blk-mq.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/atomic.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IO Delay Module");
MODULE_DESCRIPTION("IO delay via kprobe on nvme_complete_rq");
MODULE_VERSION("1.0");

// ==================== 配置参数 ====================
static unsigned long delay_ns = 10000000UL;  // 默认 10ms 延迟
module_param(delay_ns, ulong, 0644);
MODULE_PARM_DESC(delay_ns, "IO completion delay in nanoseconds");

static int delay_batch_size = 64;   // 每批处理数量
module_param(delay_batch_size, int, 0644);
MODULE_PARM_DESC(delay_batch_size, "Batch size for delayed completions");

static bool delay_enabled = true;
module_param(delay_enabled, bool, 0644);
MODULE_PARM_DESC(delay_enabled, "Enable/disable IO delay");

// ==================== 数据结构 ====================
struct delayed_req_entry {
	struct request *req;
	struct list_head list;
	ktime_t trigger_time;
};

struct io_delay_stats {
	atomic64_t total_intercepted;
	atomic64_t total_completed;
	atomic64_t total_dropped;
	atomic64_t max_queue_depth;
	atomic64_t current_queue_depth;
};

static struct io_delay_ctx {
	struct list_head delay_list;
	spinlock_t delay_lock;

	struct hrtimer delay_timer;
	ktime_t next_trigger;

	struct io_delay_stats stats;

	struct kmem_cache *entry_cache;

	void (*orig_nvme_complete_rq)(struct request *req);

	struct kprobe kp;
} g_ctx;

// ==================== 核心处理函数 ====================

static int pre_handler_nvme_complete_rq(struct kprobe *p, struct pt_regs *regs)
{
	struct request *req;
	struct delayed_req_entry *entry;
	unsigned long flags;
	ktime_t now;
	unsigned long cur_delay;
	u64 depth, max;

	if (!READ_ONCE(delay_enabled))
		return 0;

	req = (struct request *)regs->di;
	if (!req || !req->q)
		return 0;

	if (unlikely(!g_ctx.orig_nvme_complete_rq))
		return 0;

	entry = kmem_cache_alloc(g_ctx.entry_cache, GFP_ATOMIC);
	if (!entry) {
		atomic64_inc(&g_ctx.stats.total_dropped);
		return 0;
	}

	entry->req = req;
	now = ktime_get();
	cur_delay = READ_ONCE(delay_ns);
	entry->trigger_time = ktime_add_ns(now, cur_delay);

	spin_lock_irqsave(&g_ctx.delay_lock, flags);
	list_add_tail(&entry->list, &g_ctx.delay_list);
	atomic64_inc(&g_ctx.stats.total_intercepted);

	depth = atomic64_inc_return(&g_ctx.stats.current_queue_depth);
	max = atomic64_read(&g_ctx.stats.max_queue_depth);
	if (depth > max)
		atomic64_set(&g_ctx.stats.max_queue_depth, depth);

	if (g_ctx.next_trigger == 0 ||
	    ktime_compare(entry->trigger_time, g_ctx.next_trigger) < 0)
		g_ctx.next_trigger = entry->trigger_time;
	spin_unlock_irqrestore(&g_ctx.delay_lock, flags);

	hrtimer_start_range_ns(&g_ctx.delay_timer, entry->trigger_time,
				0, HRTIMER_MODE_ABS);

	return 1;
}

// 批量完成已到期的请求
static void process_expired_entries(struct list_head *expired_list, int max_count)
{
	struct delayed_req_entry *entry;
	struct delayed_req_entry *tmp;
	int count = 0;

	list_for_each_entry_safe(entry, tmp, expired_list, list) {
		if (count >= max_count)
			break;

		list_del(&entry->list);
		g_ctx.orig_nvme_complete_rq(entry->req);
		kmem_cache_free(g_ctx.entry_cache, entry);
		atomic64_inc(&g_ctx.stats.total_completed);
		atomic64_dec(&g_ctx.stats.current_queue_depth);
		count++;
	}
}

static enum hrtimer_restart delay_timer_callback(struct hrtimer *timer)
{
	struct delayed_req_entry *entry;
	struct delayed_req_entry *tmp;
	LIST_HEAD(expired_list);
	ktime_t now;
	unsigned long flags;
	ktime_t next = 0;
	bool has_more = false;

	now = ktime_get();

	// 收集所有到期的条目
	spin_lock_irqsave(&g_ctx.delay_lock, flags);

	list_for_each_entry_safe(entry, tmp, &g_ctx.delay_list, list) {
		if (ktime_compare(entry->trigger_time, now) > 0)
			break;
		list_move_tail(&entry->list, &expired_list);
	}

	// 记录下一次触发时间
	if (!list_empty(&g_ctx.delay_list)) {
		entry = list_first_entry(&g_ctx.delay_list,
					 struct delayed_req_entry, list);
		next = entry->trigger_time;
		has_more = true;
	}
	g_ctx.next_trigger = has_more ? next : 0;

	spin_unlock_irqrestore(&g_ctx.delay_lock, flags);

	// 批量处理到期请求
	process_expired_entries(&expired_list, delay_batch_size);

	// 如果还有剩余（被 max_count 截断），放回队列并重调度
	if (!list_empty(&expired_list)) {
		spin_lock_irqsave(&g_ctx.delay_lock, flags);
		list_splice(&expired_list, &g_ctx.delay_list);
		entry = list_first_entry(&g_ctx.delay_list,
					 struct delayed_req_entry, list);
		next = entry->trigger_time;
		spin_unlock_irqrestore(&g_ctx.delay_lock, flags);
		has_more = true;
	}

	if (has_more) {
		hrtimer_start(timer, next, HRTIMER_MODE_ABS);
		return HRTIMER_RESTART;
	}

	return HRTIMER_NORESTART;
}

// ==================== 模块初始化/退出 ====================

static int __init io_delay_module_init(void)
{
	int ret;

	pr_info("io_delay_kprobe: Initializing IO delay module\n");
	pr_info("io_delay_kprobe: Default delay set to %lu ns\n", delay_ns);

	INIT_LIST_HEAD(&g_ctx.delay_list);
	spin_lock_init(&g_ctx.delay_lock);

	atomic64_set(&g_ctx.stats.total_intercepted, 0);
	atomic64_set(&g_ctx.stats.total_completed, 0);
	atomic64_set(&g_ctx.stats.total_dropped, 0);
	atomic64_set(&g_ctx.stats.max_queue_depth, 0);
	atomic64_set(&g_ctx.stats.current_queue_depth, 0);

	g_ctx.entry_cache = kmem_cache_create("io_delay_entry",
					      sizeof(struct delayed_req_entry),
					      0, SLAB_HWCACHE_ALIGN, NULL);
	if (!g_ctx.entry_cache) {
		pr_err("io_delay_kprobe: Failed to create kmem cache\n");
		return -ENOMEM;
	}

	hrtimer_init(&g_ctx.delay_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	g_ctx.delay_timer.function = delay_timer_callback;
	g_ctx.next_trigger = 0;

	memset(&g_ctx.kp, 0, sizeof(struct kprobe));
	g_ctx.kp.symbol_name = "nvme_complete_rq";
	g_ctx.kp.pre_handler = pre_handler_nvme_complete_rq;

	ret = register_kprobe(&g_ctx.kp);
	if (ret < 0) {
		pr_err("io_delay_kprobe: Failed to register kprobe: %d\n", ret);
		kmem_cache_destroy(g_ctx.entry_cache);
		return ret;
	}

	g_ctx.orig_nvme_complete_rq = (void *)g_ctx.kp.addr;
	pr_info("io_delay_kprobe: Original nvme_complete_rq at %px\n",
		g_ctx.orig_nvme_complete_rq);
	pr_info("io_delay_kprobe: Successfully registered kprobe\n");

	return 0;
}

static void __exit io_delay_module_exit(void)
{
	struct delayed_req_entry *entry;
	struct delayed_req_entry *tmp;
	unsigned long flags;
	LIST_HEAD(pending_list);

	pr_info("io_delay_kprobe: Shutting down IO delay module\n");

	// 先禁用，阻止新的拦截
	WRITE_ONCE(delay_enabled, false);

	// 注销 kprobe，阻止新的 pre_handler 调用
	unregister_kprobe(&g_ctx.kp);
	pr_info("io_delay_kprobe: Kprobe unregistered\n");

	// 取消定时器，等待回调完成
	hrtimer_cancel(&g_ctx.delay_timer);
	pr_info("io_delay_kprobe: Timer cancelled\n");

	// 取出所有待处理请求（不持锁调用原始函数）
	spin_lock_irqsave(&g_ctx.delay_lock, flags);
	list_splice_init(&g_ctx.delay_list, &pending_list);
	spin_unlock_irqrestore(&g_ctx.delay_lock, flags);

	// 在锁外完成所有待处理请求
	list_for_each_entry_safe(entry, tmp, &pending_list, list) {
		list_del(&entry->list);
		if (g_ctx.orig_nvme_complete_rq)
			g_ctx.orig_nvme_complete_rq(entry->req);
		kmem_cache_free(g_ctx.entry_cache, entry);
		atomic64_dec(&g_ctx.stats.current_queue_depth);
	}

	kmem_cache_destroy(g_ctx.entry_cache);
	pr_info("io_delay_kprobe: Module unloaded\n");
}

module_init(io_delay_module_init);
module_exit(io_delay_module_exit);
