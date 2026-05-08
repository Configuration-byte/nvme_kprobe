// io_delay_kprobe.c - 通过 kprobe 拦截 nvme_complete_rq 实现 IO 延迟
//
// 可行性分析：
//   1. nvme_complete_rq 从中断上下文调用，req 此时有效
//   2. 返回 1 跳过原函数，请求留在块层 in-flight 状态
//   3. 定时器到期后，disable_kprobe → 调用原函数 → enable_kprobe
//   4. 调用者（blk_execute_rq）在栈上 wait_for_completion_io 阻塞，栈始终有效
//   5. 必须延长 req->deadline 阻止块层超时处理器触发 nvme_dev_disable
//
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

static unsigned long delay_ns = 10000000UL;
module_param(delay_ns, ulong, 0644);
MODULE_PARM_DESC(delay_ns, "IO completion delay in nanoseconds");

static int delay_batch_size = 64;
module_param(delay_batch_size, int, 0644);
MODULE_PARM_DESC(delay_batch_size, "Batch size for delayed completions");

static bool delay_enabled = true;
module_param(delay_enabled, bool, 0644);
MODULE_PARM_DESC(delay_enabled, "Enable/disable IO delay");

// ==================== 数据结构 ====================
struct delayed_req_entry {
	struct request *req;
	unsigned long deadline;		// 保存原始 deadline 用于恢复
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

typedef void (*nvme_complete_rq_fn)(struct request *req);

static struct io_delay_ctx {
	struct list_head delay_list;
	spinlock_t delay_lock;
	struct hrtimer delay_timer;
	ktime_t next_trigger;
	struct io_delay_stats stats;
	struct kmem_cache *entry_cache;
	nvme_complete_rq_fn orig_complete;
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

	entry = kmem_cache_alloc(g_ctx.entry_cache, GFP_ATOMIC);
	if (!entry) {
		atomic64_inc(&g_ctx.stats.total_dropped);
		return 0;
	}

	entry->req = req;
	entry->deadline = req->deadline;  // 保存原始 deadline

	now = ktime_get();
	cur_delay = READ_ONCE(delay_ns);
	entry->trigger_time = ktime_add_ns(now, cur_delay);

	// 关键修复：延长 deadline，阻止 blk_mq_timeout_work 触发
	//
	// 宕机链路（不加此修复时）：
	//   kprobe 返回 1 → 请求仍 in-flight
	//   → blk_mq_timeout_work 发现 deadline 到期
	//   → nvme_timeout → nvme_dev_disable → nvme_cancel_tagset
	//   → blk_mq_end_request → blk_end_sync_rq
	//   → 访问 req->end_io_data（指向 blk_execute_rq 调用者栈上的 completion）
	//   → 栈已回收 → PTE=0 → 宕机
	//
	// 修复：将 deadline 后移超过延迟时间，超时处理器不会触发
	req->deadline += nsecs_to_jiffies(cur_delay) + HZ;

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

	return 1;  // 跳过 nvme_complete_rq
}

// 调用原始 nvme_complete_rq
// 时序保证：
//   - 此时 kprobe 已被 disable_kprobe 临时移除（int3 已恢复为原指令）
//   - 调用者（blk_execute_rq）仍在栈上 wait_for_completion_io 阻塞
//   - req->deadline 已被延长，块层超时处理器不会介入
static void call_orig_complete(struct request *req)
{
	disable_kprobe(&g_ctx.kp);
	g_ctx.orig_complete(req);
	enable_kprobe(&g_ctx.kp);
}

static void process_expired_entries(struct list_head *expired_list, int max_count)
{
	struct delayed_req_entry *entry;
	struct delayed_req_entry *tmp;
	int count = 0;

	list_for_each_entry_safe(entry, tmp, expired_list, list) {
		if (count >= max_count)
			break;

		list_del(&entry->list);
		call_orig_complete(entry->req);
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

	spin_lock_irqsave(&g_ctx.delay_lock, flags);

	list_for_each_entry_safe(entry, tmp, &g_ctx.delay_list, list) {
		if (ktime_compare(entry->trigger_time, now) > 0)
			break;
		list_move_tail(&entry->list, &expired_list);
	}

	if (!list_empty(&g_ctx.delay_list)) {
		entry = list_first_entry(&g_ctx.delay_list,
					 struct delayed_req_entry, list);
		next = entry->trigger_time;
		has_more = true;
	}
	g_ctx.next_trigger = has_more ? next : 0;

	spin_unlock_irqrestore(&g_ctx.delay_lock, flags);

	process_expired_entries(&expired_list, delay_batch_size);

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

	pr_info("io_delay_kprobe: Initializing, delay=%lu ns\n", delay_ns);

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
	if (!g_ctx.entry_cache)
		return -ENOMEM;

	hrtimer_init(&g_ctx.delay_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	g_ctx.delay_timer.function = delay_timer_callback;
	g_ctx.next_trigger = 0;

	memset(&g_ctx.kp, 0, sizeof(struct kprobe));
	g_ctx.kp.symbol_name = "nvme_complete_rq";
	g_ctx.kp.pre_handler = pre_handler_nvme_complete_rq;

	ret = register_kprobe(&g_ctx.kp);
	if (ret < 0) {
		kmem_cache_destroy(g_ctx.entry_cache);
		return ret;
	}

	g_ctx.orig_complete = (nvme_complete_rq_fn)g_ctx.kp.addr;
	pr_info("io_delay_kprobe: nvme_complete_rq at %px\n", g_ctx.orig_complete);
	pr_info("io_delay_kprobe: Module loaded\n");

	return 0;
}

static void __exit io_delay_module_exit(void)
{
	struct delayed_req_entry *entry;
	struct delayed_req_entry *tmp;
	unsigned long flags;
	LIST_HEAD(pending_list);

	WRITE_ONCE(delay_enabled, false);
	unregister_kprobe(&g_ctx.kp);
	hrtimer_cancel(&g_ctx.delay_timer);

	spin_lock_irqsave(&g_ctx.delay_lock, flags);
	list_splice_init(&g_ctx.delay_list, &pending_list);
	spin_unlock_irqrestore(&g_ctx.delay_lock, flags);

	list_for_each_entry_safe(entry, tmp, &pending_list, list) {
		list_del(&entry->list);
		// kprobe 已注销，int3 已移除，直接调用
		g_ctx.orig_complete(entry->req);
		kmem_cache_free(g_ctx.entry_cache, entry);
		atomic64_dec(&g_ctx.stats.current_queue_depth);
	}

	kmem_cache_destroy(g_ctx.entry_cache);
	pr_info("io_delay_kprobe: Module unloaded\n");
}

module_init(io_delay_module_init);
module_exit(io_delay_module_exit);
