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
    // 延迟队列
    struct list_head delay_list;
    spinlock_t delay_lock;

    // 高精度定时器
    struct hrtimer delay_timer;
    ktime_t next_trigger;

    // 统计信息
    struct io_delay_stats stats;

    // 内存缓存
    struct kmem_cache *entry_cache;

    // 原函数指针
    void (*orig_nvme_complete_rq)(struct request *req);

    // kprobe 实例
    struct kprobe kp;

    // 工作队列用于批量处理
    struct work_struct complete_work;
    struct list_head work_list;
    spinlock_t work_lock;
} g_ctx;

// ==================== 核心处理函数 ====================

// 前置处理函数 - 拦截 nvme_complete_rq
static int pre_handler_nvme_complete_rq(struct kprobe *p, struct pt_regs *regs)
{
    struct request *req;
    struct delayed_req_entry *entry;
    unsigned long flags;
    ktime_t now;

    if (!delay_enabled) {
        // 功能禁用，走正常流程
        return 0;
    }

    // 从寄存器获取第一个参数 (x86_64 calling convention)
    req = (struct request *)regs->di;

    if (!req || !req->q) {
        return 0;
    }

    // 分配延迟条目
    entry = kmem_cache_alloc(g_ctx.entry_cache, GFP_ATOMIC);
    if (!entry) {
        atomic64_inc(&g_ctx.stats.total_dropped);
        return 0;
    }

    entry->req = req;
    now = ktime_get();
    entry->trigger_time = ktime_add_ns(now, (u64)delay_ns);

    // 加入延迟队列
    spin_lock_irqsave(&g_ctx.delay_lock, flags);
    list_add_tail(&entry->list, &g_ctx.delay_list);
    atomic64_inc(&g_ctx.stats.total_intercepted);

    // 更新队列深度统计
    u64 depth = atomic64_inc_return(&g_ctx.stats.current_queue_depth);
    u64 max = atomic64_read(&g_ctx.stats.max_queue_depth);
    if (depth > max) {
        atomic64_set(&g_ctx.stats.max_queue_depth, depth);
    }

    // 更新下次触发时间
    if (g_ctx.next_trigger == 0 ||
        ktime_compare(entry->trigger_time, g_ctx.next_trigger) < 0) {
        g_ctx.next_trigger = entry->trigger_time;
    }
    spin_unlock_irqrestore(&g_ctx.delay_lock, flags);

    // 如果当前时间已经超过触发时间，立即调度定时器
    if (ktime_compare(now, entry->trigger_time) >= 0) {
        hrtimer_start(&g_ctx.delay_timer, entry->trigger_time, HRTIMER_MODE_ABS);
    }

    // 返回1跳过原始函数执行
    return 1;
}

// 定时器回调 - 触发延迟完成
static enum hrtimer_restart delay_timer_callback(struct hrtimer *timer)
{
    struct delayed_req_entry *entry;
    struct delayed_req_entry *tmp;
    LIST_HEAD(expired_list);
    ktime_t now;
    unsigned long flags;
    int count = 0;

    now = ktime_get();

    // 收集所有到期的条目
    spin_lock_irqsave(&g_ctx.delay_lock, flags);
    g_ctx.next_trigger = 0;

    list_for_each_entry_safe(entry, tmp, &g_ctx.delay_list, list) {
        if (ktime_compare(entry->trigger_time, now) <= 0) {
            list_move(&entry->list, &expired_list);
        } else {
            // 更新下次触发时间
            if (g_ctx.next_trigger == 0 ||
                ktime_compare(entry->trigger_time, g_ctx.next_trigger) < 0) {
                g_ctx.next_trigger = entry->trigger_time;
            }
            break;
        }
    }
    spin_unlock_irqrestore(&g_ctx.delay_lock, flags);

    // 批量执行到期的完成
    list_for_each_entry_safe(entry, tmp, &expired_list, list) {
        list_del(&entry->list);

        // 调用原始完成函数
        if (g_ctx.orig_nvme_complete_rq)
            g_ctx.orig_nvme_complete_rq(entry->req);
        else
            pr_warn("io_delay_kprobe: orig_nvme_complete_rq is NULL!\n");

        kmem_cache_free(g_ctx.entry_cache, entry);
        atomic64_inc(&g_ctx.stats.total_completed);
        atomic64_dec(&g_ctx.stats.current_queue_depth);

        count++;

        // 超过批次大小时，延迟处理剩余的
        if (count >= delay_batch_size) {
            spin_lock_irqsave(&g_ctx.delay_lock, flags);
            list_splice(&expired_list, &g_ctx.delay_list);
            spin_unlock_irqrestore(&g_ctx.delay_lock, flags);

            // 重新调度定时器处理剩余
            hrtimer_forward(timer, now, ns_to_ktime((u64)delay_ns));
            return HRTIMER_RESTART;
        }
    }

    // 如果队列中还有条目，设置下一次定时器
    spin_lock_irqsave(&g_ctx.delay_lock, flags);
    if (!list_empty(&g_ctx.delay_list) && g_ctx.next_trigger) {
        hrtimer_start(timer, g_ctx.next_trigger, HRTIMER_MODE_ABS);
        spin_unlock_irqrestore(&g_ctx.delay_lock, flags);
        return HRTIMER_RESTART;
    }
    spin_unlock_irqrestore(&g_ctx.delay_lock, flags);

    return HRTIMER_NORESTART;
}

// ==================== 模块初始化/退出 ====================

static int __init io_delay_module_init(void)
{
    int ret;

    pr_info("io_delay_kprobe: Initializing IO delay module\n");
    pr_info("io_delay_kprobe: Default delay set to %lu ns\n", delay_ns);

    // 初始化延迟队列
    INIT_LIST_HEAD(&g_ctx.delay_list);
    INIT_LIST_HEAD(&g_ctx.work_list);
    spin_lock_init(&g_ctx.delay_lock);
    spin_lock_init(&g_ctx.work_lock);

    // 初始化统计
    atomic64_set(&g_ctx.stats.total_intercepted, 0);
    atomic64_set(&g_ctx.stats.total_completed, 0);
    atomic64_set(&g_ctx.stats.total_dropped, 0);
    atomic64_set(&g_ctx.stats.max_queue_depth, 0);
    atomic64_set(&g_ctx.stats.current_queue_depth, 0);

    // 创建内存缓存
    g_ctx.entry_cache = kmem_cache_create("io_delay_entry",
                                          sizeof(struct delayed_req_entry),
                                          0,
                                          SLAB_HWCACHE_ALIGN | SLAB_TYPESAFE_BY_RCU,
                                          NULL);
    if (!g_ctx.entry_cache) {
        pr_err("io_delay_kprobe: Failed to create kmem cache\n");
        return -ENOMEM;
    }

    // 初始化 hrtimer
    hrtimer_init(&g_ctx.delay_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
    g_ctx.delay_timer.function = delay_timer_callback;
    g_ctx.next_trigger = 0;

    // 初始化 kprobe
    memset(&g_ctx.kp, 0, sizeof(struct kprobe));
    g_ctx.kp.symbol_name = "nvme_complete_rq";
    g_ctx.kp.pre_handler = pre_handler_nvme_complete_rq;
    g_ctx.kp.post_handler = NULL;

    // 注册 kprobe
    ret = register_kprobe(&g_ctx.kp);
    if (ret < 0) {
        pr_err("io_delay_kprobe: Failed to register kprobe: %d\n", ret);
        kmem_cache_destroy(g_ctx.entry_cache);
        return ret;
    }

    // kprobe 注册成功后，kp.addr 指向原始函数地址
    g_ctx.orig_nvme_complete_rq = (void *)g_ctx.kp.addr;
    pr_info("io_delay_kprobe: Original nvme_complete_rq at %px\n", g_ctx.orig_nvme_complete_rq);

    pr_info("io_delay_kprobe: Successfully registered kprobe on nvme_complete_rq\n");
    pr_info("io_delay_kprobe: Kprobe handler at %px\n", g_ctx.kp.addr);

    return 0;
}

static void __exit io_delay_module_exit(void)
{
    struct delayed_req_entry *entry;
    struct delayed_req_entry *tmp;
    unsigned long flags;

    pr_info("io_delay_kprobe: Shutting down IO delay module\n");

    // 注销 kprobe
    unregister_kprobe(&g_ctx.kp);
    pr_info("io_delay_kprobe: Kprobe unregistered\n");

    // 取消定时器
    hrtimer_cancel(&g_ctx.delay_timer);
    pr_info("io_delay_kprobe: Timer cancelled\n");

    // 清理延迟队列 - 调用原始函数完成所有待处理的请求
    spin_lock_irqsave(&g_ctx.delay_lock, flags);
    list_for_each_entry_safe(entry, tmp, &g_ctx.delay_list, list) {
        list_del(&entry->list);
        if (g_ctx.orig_nvme_complete_rq) {
            g_ctx.orig_nvme_complete_rq(entry->req);
        }
        kmem_cache_free(g_ctx.entry_cache, entry);
        atomic64_dec(&g_ctx.stats.current_queue_depth);
    }
    spin_unlock_irqrestore(&g_ctx.delay_lock, flags);

    // 销毁内存缓存
    kmem_cache_destroy(g_ctx.entry_cache);
    pr_info("io_delay_kprobe: Cache destroyed\n");

    pr_info("io_delay_kprobe: Module unloaded\n");
}

// ==================== 导出符号 ====================

// 允许其他模块获取当前延迟值
unsigned long io_delay_get_delay_ns(void)
{
    return delay_ns;
}
EXPORT_SYMBOL_GPL(io_delay_get_delay_ns);

// 动态设置延迟值
int io_delay_set_delay(unsigned long new_delay)
{
    if (new_delay > 60000000000UL) {  // 最大 60 秒
        return -EINVAL;
    }
    delay_ns = new_delay;
    return 0;
}
EXPORT_SYMBOL_GPL(io_delay_set_delay);

module_init(io_delay_module_init);
module_exit(io_delay_module_exit);