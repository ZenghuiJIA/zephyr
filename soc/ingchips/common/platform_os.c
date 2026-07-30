/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/arch/cpu.h>
#include <zephyr/kernel.h>

#include <platform_api.h>
#include <port_gen_os_driver.h>

struct ingchips_timer {
	struct k_timer timer;
	void (*callback)(void *);
	void *user_data;
	uint32_t timeout_ms;
};

static void timer_expiry(struct k_timer *timer)
{
	struct ingchips_timer *ctx = CONTAINER_OF(timer, struct ingchips_timer, timer);

	ctx->callback(ctx->user_data);
}

static gen_handle_t timer_create(uint32_t timeout_ms, void *user_data,
				 void (*callback)(void *))
{
	struct ingchips_timer *ctx = k_malloc(sizeof(*ctx));

	if (ctx == NULL) {
		return NULL;
	}

	ctx->callback = callback;
	ctx->user_data = user_data;
	ctx->timeout_ms = timeout_ms;
	k_timer_init(&ctx->timer, timer_expiry, NULL);
	return ctx;
}

static void timer_start(gen_handle_t handle)
{
	struct ingchips_timer *ctx = handle;

	k_timer_start(&ctx->timer, K_MSEC(ctx->timeout_ms), K_NO_WAIT);
}

static void timer_stop(gen_handle_t handle)
{
	struct ingchips_timer *ctx = handle;

	k_timer_stop(&ctx->timer);
}

static void timer_delete(gen_handle_t handle)
{
	struct ingchips_timer *ctx = handle;

	k_timer_stop(&ctx->timer);
	k_free(ctx);
}

static gen_handle_t task_create(const char *name, void (*entry)(void *), void *parameter,
				uint32_t stack_size, enum gen_os_task_priority priority)
{
	struct k_thread *thread = k_malloc(sizeof(*thread));
	k_thread_stack_t *stack = k_thread_stack_alloc(stack_size, 0);
	int zephyr_priority = priority == GEN_TASK_PRIORITY_LOW ? 5 : 2;

	if (thread == NULL || stack == NULL) {
		k_free(thread);
		if (stack != NULL) {
			k_thread_stack_free(stack);
		}
		return NULL;
	}

	return k_thread_create(thread, stack, stack_size, (k_thread_entry_t)entry,
			       parameter, NULL, NULL, zephyr_priority, 0, K_NO_WAIT);
}

static gen_handle_t queue_create(int length, int item_size)
{
	struct k_msgq *queue = k_malloc(sizeof(*queue));
	char *buffer = k_malloc((size_t)length * item_size);

	if (queue == NULL || buffer == NULL) {
		k_free(queue);
		k_free(buffer);
		return NULL;
	}

	k_msgq_init(queue, buffer, item_size, length);
	return queue;
}

static int queue_send(gen_handle_t queue, void *message)
{
	return k_msgq_put(queue, message, K_NO_WAIT) == 0 ? 0 : 1;
}

static int queue_receive(gen_handle_t queue, void *message)
{
	return k_msgq_get(queue, message, K_FOREVER) == 0 ? 0 : 1;
}

static gen_handle_t event_create(void)
{
	struct k_sem *event = k_malloc(sizeof(*event));

	if (event != NULL) {
		k_sem_init(event, 0, 1);
	}
	return event;
}

static int event_wait(gen_handle_t event)
{
	return k_sem_take(event, K_FOREVER) == 0 ? 0 : 1;
}

static void event_set(gen_handle_t event)
{
	k_sem_give(event);
}

#define CRITICAL_NESTING_MAX 20
static unsigned int critical_keys[CRITICAL_NESTING_MAX];
static unsigned int critical_nesting;

static void enter_critical(void)
{
	__ASSERT_NO_MSG(critical_nesting < ARRAY_SIZE(critical_keys));
	critical_keys[critical_nesting++] = irq_lock();
}

static void leave_critical(void)
{
	__ASSERT_NO_MSG(critical_nesting > 0);
	irq_unlock(critical_keys[--critical_nesting]);
}

extern void z_cstart_prepare(void);
extern FUNC_NORETURN void z_cstart_start(void);
extern void z_arm_exc_exit(void);
extern void z_arm_pendsv(void);
extern void sys_clock_isr(void);

static FUNC_NORETURN void os_start(void)
{
	__asm__ volatile(
		"mrs r0, CONTROL\n"
		"orr r0, r0, #2\n"
		"msr CONTROL, r0\n"
		"isb\n"
		::: "r0", "memory");

	arch_irq_unlock(0);
	z_cstart_start();
}

static const gen_os_driver_t zephyr_os_driver = {
	.timer_create = timer_create,
	.timer_start = timer_start,
	.timer_stop = timer_stop,
	.timer_delete = timer_delete,
	.task_create = task_create,
	.queue_create = queue_create,
	.queue_send_msg = queue_send,
	.queue_recv_msg = queue_receive,
	.event_create = event_create,
	.event_wait = event_wait,
	.event_set = event_set,
	.malloc = k_malloc,
	.free = k_free,
	.enter_critical = enter_critical,
	.leave_critical = leave_critical,
	.os_start = os_start,
	.tick_isr = sys_clock_isr,
	.svc_isr = z_arm_exc_exit,
	.pendsv_isr = z_arm_pendsv,
};

uintptr_t app_main(void)
{
	z_cstart_prepare();
	return (uintptr_t)&zephyr_os_driver;
}
