/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * @file lib/event.c
 * @brief Non-thread-safe event handling, specific to a RADIUS server.
 *
 * @note By non-thread-safe we mean multiple threads can't insert/delete events concurrently
 *	without synchronization.
 *
 * @copyright 2007-2016 The FreeRADIUS server project
 * @copyright 2016 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 * @copyright 2007 Alan DeKok <aland@ox.org>
 */
RCSID("$Id$")

#include <freeradius-devel/libradius.h>
#include <freeradius-devel/heap.h>
#include <freeradius-devel/event.h>

#include <sys/event.h>

#define FR_EV_MAX_FDS (256)

#undef USEC
#define USEC (1000000)

/** A timer event
 *
 */
struct fr_event_timer_t {
	fr_event_callback_t	callback;	//!< Callback to execute when the timer fires.
	void			*ctx;		//!< Context pointer to pass to the callback.
	struct timeval		when;		//!< When this timer should fire.

	fr_event_timer_t	**parent;	//!< Previous timer.
	int			heap;		//!< Where to store opaque heap data.
};

/** A file descriptor event
 *
 */
typedef struct fr_event_fd_t {
	int			fd;		//!< File descriptor we're listening for events on.

	fr_event_fd_handler_t	read;		//!< callback for when data is available.
	fr_event_fd_handler_t	write;		//!< callback for when we can write data.
	fr_event_fd_handler_t	error;		//!< callback for when an error occurs on the FD.

	void			*ctx;		//!< Context pointer to pass to each callback.
} fr_event_fd_t;

/** Stores all information relating to an event list
 *
 */
struct fr_event_list_t {
	fr_heap_t	*times;			//!< of events to be executed.
	rbtree_t	*fds;			//!< Tree used to track FDs with filters in kqueue.

	int		exit;

	fr_event_status_t status;		//!< Function to call on each iteration of the event loop.

	struct timeval  now;			//!< The last time the event list was serviced.
	bool		dispatch;		//!< Whether the event list is currently dispatching events.

	int		num_readers;		//!< Number of FDs listened to by this event list.
	int		num_events;		//!< Number of events in this event list

	int		kq;			//!< instance association with this event list.


	struct kevent	events[FR_EV_MAX_FDS]; /* so it doesn't go on the stack every time */

	fr_event_fd_t	readers[FR_EV_MAX_FDS];
};

/** Compare two timer events to see which one should occur first
 *
 * @param[in] a the first timer event.
 * @param[in] b the second timer event.
 * @return
 *	- +1 if a should occur later than b.
 *	- -1 if a should occur earlier than b.
 *	- 0 if both events occur at the same time.
 */
static int fr_event_cmp_time_t(void const *a, void const *b)
{
#ifndef NDEBUG
	fr_event_timer_t const *ev_a = talloc_get_type_abort(a, fr_event_timer_t);
	fr_event_timer_t const *ev_b = talloc_get_type_abort(b, fr_event_timer_t);
#else
	fr_event_timer_t const *ev_a = a;
	fr_event_timer_t const *ev_b = b;
#endif

	if (ev_a->when.tv_sec < ev_b->when.tv_sec) return -1;
	if (ev_a->when.tv_sec > ev_b->when.tv_sec) return +1;

	if (ev_a->when.tv_usec < ev_b->when.tv_usec) return -1;
	if (ev_a->when.tv_usec > ev_b->when.tv_usec) return +1;

	return 0;
}

/** Return the number of file descriptors registered with this event loop
 *
 */
int fr_event_list_num_fds(fr_event_list_t *el)
{
	if (!el) return -1;

	return el->num_readers;
}

/** Return the number of timer events currently scheduled
 *
 * @param[in] el to return timer events for.
 * @return number of timer events.
 */
int fr_event_list_num_elements(fr_event_list_t *el)
{
	if (!el) return -1;

	return fr_heap_num_elements(el->times);
}

/** Get the current time according to the event list
 *
 * If the event list is currently dispatching events, we return the time
 * this iteration of the event list started.
 *
 * If the event list is not currently dispatching events, we return the
 * current system time.
 *
 * @param[out]	when Where to write the time we extracted/acquired.
 * @param[in]	el to get time from.
 * @return
 *	- 0 on success.
 *	- -1 on error.
 */
int fr_event_list_time(struct timeval *when, fr_event_list_t *el)
{
	if (!when) return -1;

	if (el && el->dispatch) {
		*when = el->now;
	} else {
		gettimeofday(when, NULL);
	}

	return 1;
}

/** Remove a file descriptor from the event loop
 *
 * @param[in] el	to remove file descriptor from.
 * @param[in] fd	to remove.
 * @return
 *	- 0 if file descriptor was removed.
 *	- <0 on error.
 */
int fr_event_fd_delete(fr_event_list_t *el, int fd)
{
	int i;

	if (!el || (fd < 0)) return -1;

	for (i = 0; i < FR_EV_MAX_FDS; i++) {
		int j;
		struct kevent evset;

		j = (i + fd) & (FR_EV_MAX_FDS - 1);

		if (el->readers[j].fd != fd) continue;

		/*
		 *	Tell the kernel to delete it from the list.
		 *
		 *	The caller MAY have closed it, in which case
		 *	the kernel has removed it from the list.  So
		 *	we ignore the return code from kevent().
		 */
		EV_SET(&evset, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
		(void) kevent(el->kq, &evset, 1, NULL, 0, NULL);

		el->readers[j].fd = -1;
		el->num_readers--;

		return 0;
	}


	return -1;
}

/** Associate a callback with an FD
 *
 * @param[in] el	to insert FD callback into.
 * @param[in] fd	to read from.
 * @param[in] read	function to call when fd is readable.
 * @param[in] write	function to call when fd is writable.
 * @param[in] error	function to call when an error occurs on the fd.
 * @param[in] ctx	to pass to handler.
 */
int fr_event_fd_insert(fr_event_list_t *el, int fd,
		       fr_event_fd_handler_t read, UNUSED fr_event_fd_handler_t write, UNUSED fr_event_fd_handler_t error,
		       void *ctx)
{
	int i;
	fr_event_fd_t *ef;

	if (!el) {
		fr_strerror_printf("Invalid argument: NULL event list");
		return -1;
	}

	if (!read) {
		fr_strerror_printf("Invalid arguments: NULL handler");
		return -1;
	}

	if (fd < 0) {
		fr_strerror_printf("Invalid arguments: Bad FD %i", fd);
		return -1;
	}

	if (el->num_readers >= FR_EV_MAX_FDS) {
		fr_strerror_printf("Too many readers");
		return -1;
	}
	ef = NULL;

	/*
	 *	We need to store TWO fields with the event.  kqueue
	 *	only lets us store one.  If we put the two fields into
	 *	a heap allocated structure, that would help.  Except that
	 *	kqueue can silently delete the event when the socket
	 *	is closed, and not give us the opportunity to free it.
	 *	<sigh>
	 *
	 *	The solution is to put the fields into an array, and
	 *	do a linear search on addition/deletion of the FDs.
	 *	However, to avoid MOST linear issues, we start off the
	 *	search at "FD" offset.  Since FDs are unique, AND
	 *	usually less than 256, we do "FD & 0xff", which is a
	 *	good guess, and makes the lookups mostly O(1).
	 */
	for (i = 0; i < FR_EV_MAX_FDS; i++) {
		int j;
		struct kevent evset;

		j = (i + fd) & (FR_EV_MAX_FDS - 1);

		if (el->readers[j].fd >= 0) continue;

		/*
		 *	We want to read from the FD.
		 */
		EV_SET(&evset, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, &el->readers[j]);
		if (kevent(el->kq, &evset, 1, NULL, 0, NULL) < 0) {
			fr_strerror_printf("Failed inserting event for FD %i: %s", fd, fr_syserror(errno));
			return -1;
		}

		ef = &el->readers[j];
		el->num_readers++;
		break;
	}

	if (!ef) {
		fr_strerror_printf("Failed assigning FD");
		return -1;
	}

	ef->fd = fd;
	ef->read = read;
	ef->ctx = ctx;

	return 0;
}


/** Delete a timer event from the event list
 *
 * @param[in] el	to delete event from.
 * @param[in] parent	of the event being deleted.
 */
int fr_event_timer_delete(fr_event_list_t *el, fr_event_timer_t **parent)
{
	int ret;

	fr_event_timer_t *ev;

	if (!el || !parent || !*parent) return -1;

#ifndef NDEBUG
	/*
	 *  Validate the event_t struct to detect memory issues early.
	 */
	ev = talloc_get_type_abort(*parent, fr_event_timer_t);

#else
	ev = *parent;
#endif

	if (ev->parent) {
		(void)fr_cond_assert(*(ev->parent) == ev);
		*ev->parent = NULL;
	}
	*parent = NULL;

	ret = fr_heap_extract(el->times, ev);
	(void)fr_cond_assert(ret == 1);	/* events MUST be in the heap */
	talloc_free(ev);

	return ret;
}

/** Insert a timer event into an event list
 *
 * @param[in] el	to insert event into.
 * @param[in] callback	function to execute if the event fires.
 * @param[in] ctx	for callback function.
 * @param[in] when	we should run the event.
 * @param[in] parent	If not NULL modify this event instead of creating a new one.  This is a parent
 *			in a temporal sense, not in a memory structure or dependency sense.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_event_timer_insert(fr_event_list_t *el, fr_event_callback_t callback, void *ctx,
			  struct timeval *when, fr_event_timer_t **parent)
{
	fr_event_timer_t *ev;

	if (!el) {
		fr_strerror_printf("Invalid arguments: NULL event list");
		return -1;
	}

	if (!callback) {
		fr_strerror_printf("Invalid arguments: NULL callback");
		return -1;
	}

	if (!when || (when->tv_usec >= USEC)) {
		fr_strerror_printf("Invalid arguments: time");
		return -1;
	}

	if (!parent) {
		fr_strerror_printf("Invalid arguments: NULL parent");
		return -1;
	}

	/*
	 *	If there is an event, re-use it instead of freeing it
	 *	and allocating a new one.
	 */
	if (*parent) {
		int ret;

#ifndef NDEBUG
		ev = talloc_get_type_abort(*parent, fr_event_timer_t);
#else
		ev = *parent;
#endif

		ret = fr_heap_extract(el->times, ev);
		if (!fr_cond_assert(ret == 1)) return -1;	/* events MUST be in the heap */

		memset(ev, 0, sizeof(*ev));
	} else {
		ev = talloc_zero(el, fr_event_timer_t);
		if (!ev) return -1;
	}

	ev->callback = callback;
	ev->ctx = ctx;
	ev->when = *when;
	ev->parent = parent;

	if (!fr_heap_insert(el->times, ev)) {
		fr_strerror_printf("Failed inserting event into heap");
		talloc_free(ev);
		return -1;
	}

	*parent = ev;

	return 0;
}

/** Run a single scheduled timer event
 *
 * @param[in] el	containing the timer events.
 * @param[in] when	Process events scheduled to run before or at this time.
 * @return
 *	- 0 no timer events fired.
 *	- 1 a timer event fired.
 */
int fr_event_timer_run(fr_event_list_t *el, struct timeval *when)
{
	fr_event_callback_t callback;
	void *ctx;
	fr_event_timer_t *ev;

	if (!el) return 0;

	if (fr_heap_num_elements(el->times) == 0) {
		when->tv_sec = 0;
		when->tv_usec = 0;
		return 0;
	}

	ev = fr_heap_peek(el->times);
	if (!ev) {
		when->tv_sec = 0;
		when->tv_usec = 0;
		return 0;
	}

	/*
	 *	See if it's time to do this one.
	 */
	if ((ev->when.tv_sec > when->tv_sec) ||
	    ((ev->when.tv_sec == when->tv_sec) &&
	     (ev->when.tv_usec > when->tv_usec))) {
		*when = ev->when;
		return 0;
	}

	callback = ev->callback;
	ctx = ev->ctx;

	/*
	 *	Delete the event before calling it.
	 */
	fr_event_timer_delete(el, ev->parent);

	callback(ctx, when);

	return 1;
}

/** Gather outstanding timer and file descriptor events
 *
 * @param[in] el	to process events for.
 * @param[in] wait	if true, block on the kevent() call until a timer or file descriptor event occurs.
 * @return the number of outstanding events.
 */
int fr_event_corral(fr_event_list_t *el, bool wait)
{
	struct timeval when, *wake;
	struct timespec ts_when, *ts_wake;

	/*
	 *	Find the first event.  If there's none, we wait
	 *	on the socket forever.
	 */
	when.tv_sec = 0;
	when.tv_usec = 0;
	wake = &when;

	if (wait) {
		if (fr_heap_num_elements(el->times) > 0) {
			fr_event_timer_t *ev;

			ev = fr_heap_peek(el->times);
			if (!fr_cond_assert(ev)) return -1;

			gettimeofday(&el->now, NULL);

			/*
			 *	Next event is in the future, get the time
			 *	between now and that event.
			 */
			if (fr_timeval_cmp(&ev->when, &el->now) > 0) fr_timeval_subtract(&when, &ev->when, &el->now);
		} else {
			wake = NULL;
		}
	}

	/*
	 *	Run the status callback
	 */
	if (el->status) el->status(wake);

	if (wake) {
		ts_wake = &ts_when;
		ts_when.tv_sec = when.tv_sec;
		ts_when.tv_nsec = when.tv_usec * 1000;
	} else {
		ts_wake = NULL;
	}

	/*
	 *	Populate el->events with the list of I/O events
	 *	that occurred since this function was last occurred
	 *	or wait for the next timer event.
	 */
	el->num_events = kevent(el->kq, NULL, 0, el->events, FR_EV_MAX_FDS, ts_wake);

	/*
	 *	Interrupt is different from timeout / FD events.
	 */
	if ((el->num_events < 0) && (errno == EINTR)) el->num_events = 0;

	return el->num_events;
}

/** Service any outstanding timer or file descriptor events
 *
 * @param[in] el containing events to service.
 */
void fr_event_service(fr_event_list_t *el)
{
	int i;

	/*
	 *	Loop over all of the events, servicing them.
	 */
	for (i = 0; i < el->num_events; i++) {
		fr_event_fd_t *ef = el->events[i].udata;

		if (el->events[i].flags & EV_EOF) {
			/*
			 *	FIXME: delete the handler
			 *	here, and fix process.c to not
			 *	call fr_event_fd_delete().
			 *	It's cleaner.
			 *
			 *	Call the handler, which SHOULD
			 *	delete the connection.
			 */
			ef->read(el, ef->fd, ef->ctx);
			continue;
		}

		/*
		 *	Else it's our event.  We only set
		 *	EVFILT_READ, so it must be a read
		 *	event.
		 */
		ef->read(el, ef->fd, ef->ctx);
	}

	if (fr_heap_num_elements(el->times) > 0) {
		struct timeval when;

		do {
			gettimeofday(&el->now, NULL);
			when = el->now;
		} while (fr_event_timer_run(el, &when) == 1);
	}
}

/** Signal an event loop exit with the specified code
 *
 * The event loop will complete its current iteration, and then exit with the specified code.
 *
 * @param[in] el	to signal to exit.
 * @param[in] code	for #fr_event_loop to return.
 */
void fr_event_loop_exit(fr_event_list_t *el, int code)
{
	if (!el) return;

	el->exit = code;
}

/** Check to see whether the event loop is in the process of exiting
 *
 * @param[in] el	to check.
 */
bool fr_event_loop_exiting(fr_event_list_t *el)
{
	return (el->exit != 0);
}

/** Run an event loop
 *
 * @note Will not return until #fr_event_loop_exit is called.
 *
 * @param[in] el to start processing.
 */
int fr_event_loop(fr_event_list_t *el)
{
	el->exit = 0;
	el->dispatch = true;

	while (!el->exit) {
		if (fr_event_corral(el, true) < 0) break;

		fr_event_service(el);
	}

	el->dispatch = false;
	return el->exit;
}

/** Cleanup an event list
 *
 * Frees/destroys any resources associated with an event list
 *
 * @param[in] el to free resources for.
 */
static int _event_list_free(fr_event_list_t *el)
{
	fr_event_timer_t *ev;

	while ((ev = fr_heap_peek(el->times)) != NULL) {
		fr_event_timer_delete(el, &ev);
	}

	fr_heap_delete(el->times);

	close(el->kq);

	return 0;
}

/** Initialise a new event list
 *
 * @param[in] ctx	to allocate memory in.
 * @param[in] status	callback, called on each iteration of the event list.
 * @return
 *	- A pointer to a new event list on success (free with talloc_free).
 *	- NULL on error.
 */
fr_event_list_t *fr_event_list_init(TALLOC_CTX *ctx, fr_event_status_t status)
{
	int i;
	fr_event_list_t *el;

	el = talloc_zero(ctx, fr_event_list_t);
	if (!fr_cond_assert(el)) {
		return NULL;
	}
	talloc_set_destructor(el, _event_list_free);

	el->times = fr_heap_create(fr_event_cmp_time_t, offsetof(fr_event_timer_t, heap));
	if (!el->times) {
		talloc_free(el);
		return NULL;
	}

	for (i = 0; i < FR_EV_MAX_FDS; i++) {
		el->readers[i].fd = -1;
	}

	el->kq = kqueue();
	if (el->kq < 0) {
		talloc_free(el);
		return NULL;
	}

	el->status = status;

	return el;
}

#ifdef TESTING

/*
 *  cc -g -I .. -c rbtree.c -o rbtree.o && cc -g -I .. -c isaac.c -o isaac.o && cc -DTESTING -I .. -c event.c  -o event_mine.o && cc event_mine.o rbtree.o isaac.o -o event
 *
 *  ./event
 *
 *  And hit CTRL-S to stop the output, CTRL-Q to continue.
 *  It normally alternates printing the time and sleeping,
 *  but when you hit CTRL-S/CTRL-Q, you should see a number
 *  of events run right after each other.
 *
 *  OR
 *
 *   valgrind --tool=memcheck --leak-check=full --show-reachable=yes ./event
 */

static void print_time(void *ctx)
{
	struct timeval *when = ctx;

	printf("%d.%06d\n", when->tv_sec, when->tv_usec);
	fflush(stdout);
}

static fr_randctx rand_pool;

static uint32_t event_rand(void)
{
	uint32_t num;

	num = rand_pool.randrsl[rand_pool.randcnt++];
	if (rand_pool.randcnt == 256) {
		fr_isaac(&rand_pool);
		rand_pool.randcnt = 0;
	}

	return num;
}


#define MAX 100
int main(int argc, char **argv)
{
	int i, rcode;
	struct timeval array[MAX];
	struct timeval now, when;
	fr_event_list_t *el;

	el = fr_event_list_init(NULL, NULL);
	if (!el) exit(1);

	memset(&rand_pool, 0, sizeof(rand_pool));
	rand_pool.randrsl[1] = time(NULL);

	fr_randinit(&rand_pool, 1);
	rand_pool.randcnt = 0;

	gettimeofday(&array[0], NULL);
	for (i = 1; i < MAX; i++) {
		array[i] = array[i - 1];

		array[i].tv_usec += event_rand() & 0xffff;
		if (array[i].tv_usec > 1000000) {
			array[i].tv_usec -= 1000000;
			array[i].tv_sec++;
		}
		fr_event_timer_insert(el, print_time, &array[i], &array[i]);
	}

	while (fr_event_list_num_elements(el)) {
		gettimeofday(&now, NULL);
		when = now;
		if (!fr_event_timer_run(el, &when)) {
			int delay = (when.tv_sec - now.tv_sec) * 1000000;
			delay += when.tv_usec;
			delay -= now.tv_usec;

			printf("\tsleep %d\n", delay);
			fflush(stdout);
			usleep(delay);
		}
	}

	talloc_free(el);

	return 0;
}
#endif
