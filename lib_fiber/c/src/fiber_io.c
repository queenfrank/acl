#include "stdafx.h"
#include "common.h"

#include "fiber/lib_fiber.h"
#include "common/gettimeofday.h"
#include "event.h"
#include "fiber.h"

typedef struct {
	EVENT     *event;
	size_t     io_count;
	ACL_FIBER *ev_fiber;
	RING       ev_timer;
	int        nsleeping;
	int        io_stop;
	FILE_EVENT **events;
} FIBER_TLS;

static FIBER_TLS *__main_fiber = NULL;
static __thread FIBER_TLS *__thread_fiber = NULL;

static void fiber_io_loop(ACL_FIBER *fiber, void *ctx);

#define MAXFD		1024
#define STACK_SIZE	819200

socket_t var_maxfd = 1024;

void acl_fiber_schedule_stop(void)
{
	fiber_io_check();
	__thread_fiber->io_stop = 1;
}

#define RING_TO_FIBER(r) \
	((ACL_FIBER *) ((char *) (r) - offsetof(ACL_FIBER, me)))

#define FIRST_FIBER(head) \
	(ring_succ(head) != (head) ? RING_TO_FIBER(ring_succ(head)) : 0)

static pthread_key_t __fiber_key;

static void thread_free(void *ctx)
{
	FIBER_TLS *tf = (FIBER_TLS *) ctx;

	if (__thread_fiber == NULL) {
		return;
	}

	if (tf->event) {
		event_free(tf->event);
		tf->event = NULL;
	}

	free(tf->events);
	free(tf);

	if (__main_fiber == __thread_fiber) {
		__main_fiber = NULL;
	}
	__thread_fiber = NULL;
}

static void fiber_io_main_free(void)
{
	if (__main_fiber) {
		thread_free(__main_fiber);
		if (__thread_fiber == __main_fiber) {
			__thread_fiber = NULL;
		}
		__main_fiber = NULL;
	}
}

static void thread_init(void)
{
	if (pthread_key_create(&__fiber_key, thread_free) != 0) {
		msg_fatal("%s(%d), %s: pthread_key_create error %s",
			__FILE__, __LINE__, __FUNCTION__, last_serror());
	}
}

static pthread_once_t __once_control = PTHREAD_ONCE_INIT;

void fiber_io_check(void)
{
	if (__thread_fiber != NULL) {
		return;
	}

	if (pthread_once(&__once_control, thread_init) != 0) {
		msg_fatal("%s(%d), %s: pthread_once error %s",
			__FILE__, __LINE__, __FUNCTION__, last_serror());
	}

	var_maxfd = open_limit(0);
	if (var_maxfd <= 0) {
		var_maxfd = MAXFD;
	}

	__thread_fiber = (FIBER_TLS *) malloc(sizeof(FIBER_TLS));
	__thread_fiber->event = event_create(var_maxfd);
	__thread_fiber->ev_fiber = acl_fiber_create(fiber_io_loop,
			__thread_fiber->event, STACK_SIZE);
	__thread_fiber->io_count = 0;
	__thread_fiber->nsleeping = 0;
	__thread_fiber->io_stop = 0;
	ring_init(&__thread_fiber->ev_timer);

	__thread_fiber->events = (FILE_EVENT **)
		calloc(var_maxfd, sizeof(FILE_EVENT*));

	if (__pthread_self() == main_thread_self()) {
		__main_fiber = __thread_fiber;
		atexit(fiber_io_main_free);
	} else if (pthread_setspecific(__fiber_key, __thread_fiber) != 0) {
		msg_fatal("pthread_setspecific error!");
	}
}

void fiber_io_dec(void)
{
	fiber_io_check();
	__thread_fiber->io_count--;
}

void fiber_io_inc(void)
{
	fiber_io_check();
	__thread_fiber->io_count++;
}

EVENT *fiber_io_event(void)
{
	fiber_io_check();
	return __thread_fiber->event;
}

static void fiber_io_loop(ACL_FIBER *self fiber_unused, void *ctx)
{
	EVENT *ev = (EVENT *) ctx;
	ACL_FIBER *timer;
	long long now, last = 0, left;

	fiber_system();

	for (;;) {
		while (acl_fiber_yield() > 0) {}

		timer = FIRST_FIBER(&__thread_fiber->ev_timer);
		if (timer == NULL) {
			left = -1;
		} else {
			SET_TIME(now);
			last = now;
			if (now >= timer->when)
				left = 0;
			else
				left = timer->when - now;
		}

		assert(left < INT_MAX);

		/* add 1 just for the deviation of epoll_wait */
		event_process(ev, left > 0 ? (int) left + 1 : (int) left);

		if (__thread_fiber->io_stop) {
			break;
		}

		if (timer == NULL) {
			continue;
		}

		SET_TIME(now);

		if (now - last < left) {
			continue;
		}

		do {
			ring_detach(&timer->me);

			if (!timer->sys && --__thread_fiber->nsleeping == 0) {
				fiber_count_dec();
			}

			acl_fiber_ready(timer);
			timer = FIRST_FIBER(&__thread_fiber->ev_timer);

		} while (timer != NULL && now >= timer->when);
	}

	if (__thread_fiber->io_count > 0) {
		msg_info("%s(%d), %s: waiting io: %d", __FILE__, __LINE__,
			__FUNCTION__, (int) __thread_fiber->io_count);
	}
}

#define CHECK_MIN

unsigned int acl_fiber_delay(unsigned int milliseconds)
{
	long long when, now;
	ACL_FIBER *fiber;
	RING_ITER iter;
	EVENT *ev;
#ifdef	CHECK_MIN
	long long min = -1;
#endif

	if (!var_hook_sys_api) {
		doze(milliseconds);
		return 0;
	}

	fiber_io_check();

	ev = fiber_io_event();

	SET_TIME(when);
	when += milliseconds;

	ring_foreach_reverse(iter, &__thread_fiber->ev_timer) {
		fiber = ring_to_appl(iter.ptr, ACL_FIBER, me);
		if (when >= fiber->when) {
#ifdef	CHECK_MIN
			long long n = when - fiber->when;
			if (min == -1 || n < min) {
				min = n;
			}
#endif
			break;
		}
	}

#ifdef	CHECK_MIN
	if ((min >= 0 && min < ev->timeout) || ev->timeout < 0) {
		ev->timeout = (int) min;
	}
#else
	ev->timeout = 10;
#endif

	fiber = acl_fiber_running();
	fiber->when = when;
	ring_detach(&fiber->me);

	ring_append(iter.ptr, &fiber->me);

	if (!fiber->sys && __thread_fiber->nsleeping++ == 0) {
		fiber_count_inc();
	}

	acl_fiber_switch();

	//ring_detach(&fiber->me);

	if (ring_size(&__thread_fiber->ev_timer) == 0) {
		ev->timeout = -1;
	} else {
		ev->timeout = (int) min;
	}

	SET_TIME(now);
	if (now < when)
		return 0;

	return (unsigned int) (now - when);
}

static void fiber_timer_callback(ACL_FIBER *fiber, void *ctx)
{
	long long now, left;

	SET_TIME(now);

	for (;;) {
		left = fiber->when > now ? fiber->when - now : 0;
		if (left == 0) {
			break;
		}

		acl_fiber_delay((unsigned int) left);

		SET_TIME(now);
		if (fiber->when <= now) {
			break;
		}
	}

	fiber->timer_fn(fiber, ctx);
	fiber_exit(0);
}

ACL_FIBER *acl_fiber_create_timer(unsigned int milliseconds, size_t size,
	void (*fn)(ACL_FIBER *, void *), void *ctx)
{
	long long when;
	ACL_FIBER *fiber;

	fiber_io_check();

	SET_TIME(when);
	when += milliseconds;

	fiber           = acl_fiber_create(fiber_timer_callback, ctx, size);
	fiber->when     = when;
	fiber->timer_fn = fn;
	return fiber;
}

void acl_fiber_reset_timer(ACL_FIBER *fiber, unsigned int milliseconds)
{
	long long when;

	fiber_io_check();

	SET_TIME(when);
	when += milliseconds;
	fiber->when = when;
	fiber->status = FIBER_STATUS_READY;
}

unsigned int acl_fiber_sleep(unsigned int seconds)
{
	return acl_fiber_delay(seconds * 1000) / 1000;
}

static void read_callback(EVENT *ev, FILE_EVENT *fe)
{
	event_del_read(ev, fe);
	acl_fiber_ready(fe->fiber);
	__thread_fiber->io_count--;
}

void fiber_wait_read(FILE_EVENT *fe)
{
	fiber_io_check();

	fe->fiber = acl_fiber_running();
	// when return 0 just let it go continue
	if (!event_add_read(__thread_fiber->event, fe, read_callback))
		return;
	__thread_fiber->io_count++;
	acl_fiber_switch();
}

static void write_callback(EVENT *ev, FILE_EVENT *fe)
{
	event_del_write(ev, fe);
	acl_fiber_ready(fe->fiber);
	__thread_fiber->io_count--;
}

void fiber_wait_write(FILE_EVENT *fe)
{
	fiber_io_check();

	fe->fiber = acl_fiber_running();
	if (!event_add_write(__thread_fiber->event, fe, write_callback))
		return;
	__thread_fiber->io_count++;
	acl_fiber_switch();
}

/****************************************************************************/

static FILE_EVENT *fiber_file_get(socket_t fd)
{
	fiber_io_check();
	if (fd < 0 || fd >= var_maxfd) {
		msg_error("%s(%d): invalid fd=%d",
			__FUNCTION__, __LINE__, fd);
		return NULL;
	}

	return __thread_fiber->events[fd];
}

static void fiber_file_set(FILE_EVENT *fe)
{
	if (fe->fd == INVALID_SOCKET || fe->fd >= (socket_t) var_maxfd) {
		msg_fatal("%s(%d): invalid fd=%d",
			__FUNCTION__, __LINE__, fe->fd);
	}

	if (__thread_fiber->events[fe->fd] != NULL) {
		msg_fatal("%s(%d): exist fd=%d",
			__FUNCTION__, __LINE__, fe->fd);
	}

	__thread_fiber->events[fe->fd] = fe;
}

FILE_EVENT *fiber_file_open(socket_t fd)
{
	FILE_EVENT *fe = fiber_file_get(fd);

	if (fe == NULL) {
		fe = file_event_alloc(fd);
		fiber_file_set(fe);
	}
	return fe;
}

static int fiber_file_del(FILE_EVENT *fe)
{
	if (fe->fd == INVALID_SOCKET || fe->fd >= var_maxfd) {
		msg_error("%s(%d): invalid fd=%d",
			__FUNCTION__, __LINE__, fe->fd);
		return -1;
	}

	if (__thread_fiber->events[fe->fd] != fe) {
		msg_error("%s(%d): invalid fe=%p, fd=%d, origin=%p",
			__FUNCTION__, __LINE__, fe, fe->fd,
			__thread_fiber->events[fe->fd]);
		return -1;
	}

	__thread_fiber->events[fe->fd] = NULL;
	return 0;
}

int fiber_file_close(socket_t fd)
{
	FILE_EVENT *fe;

	fiber_io_check();
	if (fd < 0 || fd >= var_maxfd) {
		msg_error("%s(%d): invalid fd=%d", __FUNCTION__, __LINE__, fd);
		return -1;
	}

	fe = fiber_file_get(fd);
	if (fe == NULL) {
		return 0;
	}

	event_close(__thread_fiber->event, fe);
	fiber_file_del(fe);
	file_event_free(fe);

	return 1;
}
