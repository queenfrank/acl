#include "stdafx.h"
#include "common.h"

#include "fiber.h"

#ifdef SYS_WIN
typedef ssize_t (__stdcall *recv_fn)(socket_t, char *, int, int);
typedef ssize_t (__stdcall *recvfrom_fn)(socket_t, char *, int, int,
	struct sockaddr *, socklen_t *);
typedef ssize_t (__stdcall *send_fn)(socket_t, const char *, int, int);
typedef ssize_t (__stdcall *sendto_fn)(socket_t, const char *, int, int,
	const struct sockaddr *, socklen_t);
#else
typedef unsigned int (*sleep_fn)(unsigned int seconds);
typedef int     (*close_fn)(int);
typedef ssize_t (*read_fn)(socket_t, void *, size_t);
typedef ssize_t (*readv_fn)(socket_t, const struct iovec *, int);
typedef ssize_t (*recv_fn)(socket_t, void *, size_t, int);
typedef ssize_t (*recvfrom_fn)(socket_t, void *, size_t, int,
	struct sockaddr *, socklen_t *);
typedef ssize_t (*recvmsg_fn)(socket_t, struct msghdr *, int);
typedef ssize_t (*write_fn)(socket_t, const void *, size_t);
typedef ssize_t (*writev_fn)(socket_t, const struct iovec *, int);
typedef ssize_t (*send_fn)(socket_t, const void *, size_t, int);
typedef ssize_t (*sendto_fn)(socket_t, const void *, size_t, int,
	const struct sockaddr *, socklen_t);
typedef ssize_t (*sendmsg_fn)(socket_t, const struct msghdr *, int);
#ifdef  __USE_LARGEFILE64
typedef ssize_t (*sendfile64_fn)(socket_t, int, off64_t*, size_t);
#endif
#endif

#ifdef SYS_UNIX
static sleep_fn    __sys_sleep    = NULL;
static close_fn    __sys_close    = NULL;
static read_fn     __sys_read     = NULL;
static readv_fn    __sys_readv    = NULL;
static recvmsg_fn  __sys_recvmsg  = NULL;

static write_fn    __sys_write    = NULL;
static writev_fn   __sys_writev   = NULL;
static sendmsg_fn  __sys_sendmsg  = NULL;
#endif
static recv_fn     __sys_recv     = NULL;
static recvfrom_fn __sys_recvfrom = NULL;

static send_fn     __sys_send     = NULL;
static sendto_fn   __sys_sendto   = NULL;

#ifdef __USE_LARGEFILE64
static sendfile64_fn __sys_sendfile64 = NULL;
#endif

static void hook_init(void)
{
#ifdef SYS_UNIX
	static pthread_mutex_t __lock = PTHREAD_MUTEX_INITIALIZER;
	static int __called = 0;

	(void) pthread_mutex_lock(&__lock);

	if (__called) {
		(void) pthread_mutex_unlock(&__lock);
		return;
	}

	__called++;

	__sys_sleep      = (sleep_fn) dlsym(RTLD_NEXT, "sleep");
	assert(__sys_sleep);

	__sys_close      = (close_fn) dlsym(RTLD_NEXT, "close");
	assert(__sys_close);

	__sys_read       = (read_fn) dlsym(RTLD_NEXT, "read");
	assert(__sys_read);

	__sys_readv      = (readv_fn) dlsym(RTLD_NEXT, "readv");
	assert(__sys_readv);

	__sys_recv       = (recv_fn) dlsym(RTLD_NEXT, "recv");
	assert(__sys_recv);

	__sys_recvfrom   = (recvfrom_fn) dlsym(RTLD_NEXT, "recvfrom");
	assert(__sys_recvfrom);

	__sys_recvmsg    = (recvmsg_fn) dlsym(RTLD_NEXT, "recvmsg");
	assert(__sys_recvmsg);

	__sys_write      = (write_fn) dlsym(RTLD_NEXT, "write");
	assert(__sys_write);

	__sys_writev     = (writev_fn) dlsym(RTLD_NEXT, "writev");
	assert(__sys_writev);

	__sys_send       = (send_fn) dlsym(RTLD_NEXT, "send");
	assert(__sys_send);

	__sys_sendto     = (sendto_fn) dlsym(RTLD_NEXT, "sendto");
	assert(__sys_sendto);

	__sys_sendmsg    = (sendmsg_fn) dlsym(RTLD_NEXT, "sendmsg");
	assert(__sys_sendmsg);

#ifdef __USE_LARGEFILE64
	__sys_sendfile64 = (sendfile64_fn) dlsym(RTLD_NEXT, "sendfile64");
	assert(__sys_sendfile64);
#endif

	(void) pthread_mutex_unlock(&__lock);
#else
	__sys_recv     = recv;
	__sys_recvfrom = recvfrom;
	__sys_send     = send;
	__sys_sendto   = sendto;
#endif
}

#ifdef SYS_UNIX
unsigned int sleep(unsigned int seconds)
{
	if (!var_hook_sys_api) {
		if (__sys_sleep == NULL) {
			hook_init();
		}

		return __sys_sleep(seconds);
	}

	return acl_fiber_sleep(seconds);
}

int close(socket_t fd)
{
	int ret;

	if (fd == INVALID_SOCKET) {
		msg_error("%s: invalid fd: %d", __FUNCTION__, fd);
		return -1;
	}

	if (__sys_close == NULL) {
		hook_init();
	}

	if (!var_hook_sys_api) {
		return __sys_close(fd);
	}

	/* when the fd was closed by epoll_event_close normally, the fd
	 * must be a epoll fd which was created by epoll_create function
	 * hooked in hook_net.c
	 */
#ifdef	HAS_EPOLL
	if (epoll_event_close(fd) == 0) {
		return 0;
	}
#elif	defined(HAS_KQUEUE)
#endif

	fiber_file_close(fd);
	ret = __sys_close(fd);
	if (ret == 0) {
		return ret;
	}

	fiber_save_errno();
	return ret;
}
#endif

/****************************************************************************/

#define READ_WAIT_FIRST

#ifdef READ_WAIT_FIRST

# ifdef SYS_UNIX
ssize_t fiber_read(socket_t fd, void *buf, size_t count)
{
	ssize_t ret;
	FILE_EVENT* fe;

	if (fd == INVALID_SOCKET) {
		msg_error("%s: invalid fd: %d", __FUNCTION__, fd);
		return -1;
	}

	if (__sys_read == NULL) {
		hook_init();
	}

	if (!var_hook_sys_api) {
		return __sys_read(fd, buf, count);
	}

	fe = fiber_file_open(fd);
	fiber_wait_read(fe);

	ret = __sys_read(fd, buf, count);
	if (ret >= 0) {
		return ret;
	}

	fiber_save_errno();

	if (acl_fiber_killed(fe->fiber)) {
		msg_info("%s(%d), %s: fiber-%u is existing", __FILE__,
			__LINE__, __FUNCTION__, acl_fiber_id(fe->fiber));
	}

	return ret;
}

ssize_t fiber_readv(socket_t fd, const struct iovec *iov, int iovcnt)
{
	ssize_t ret;
	FILE_EVENT *fe;

	if (fd == INVALID_SOCKET) {
		msg_error("%s: invalid fd: %d", __FUNCTION__, fd);
		return -1;
	}

	if (__sys_readv == NULL) {
		hook_init();
	}

	if (!var_hook_sys_api) {
		return __sys_readv(fd, iov, iovcnt);
	}

	fe = fiber_file_open(fd);
	fiber_wait_read(fe);

	ret = __sys_readv(fd, iov, iovcnt);
	if (ret >= 0) {
		return ret;
	}

	fiber_save_errno();

	if (acl_fiber_killed(fe->fiber)) {
		msg_info("%s(%d), %s: fiber-%u is existing", __FILE__,
			__LINE__, __FUNCTION__, acl_fiber_id(fe->fiber));
	}

	return ret;
}
# endif

ssize_t fiber_recv(socket_t sockfd, void *buf, size_t len, int flags)
{
	ssize_t ret;
	FILE_EVENT *fe;

	if (sockfd == INVALID_SOCKET) {
		msg_error("%s: invalid sockfd: %d", __FUNCTION__, sockfd);
		return -1;
	}

	if (__sys_recv == NULL) {
		hook_init();
	}

	if (!var_hook_sys_api) {
		return __sys_recv(sockfd, buf, len, flags);
	}

	fe = fiber_file_open(sockfd);
	fiber_wait_read(fe);

	ret = __sys_recv(sockfd, buf, len, flags);
	if (ret >= 0) {
		return ret;
	}

	fiber_save_errno();

	if (acl_fiber_killed(fe->fiber)) {
		msg_info("%s(%d), %s: fiber-%u is existing", __FILE__,
			__LINE__, __FUNCTION__, acl_fiber_id(fe->fiber));
	}

	return ret;
}

ssize_t fiber_recvfrom(socket_t sockfd, void *buf, size_t len, int flags,
	struct sockaddr *src_addr, socklen_t *addrlen)
{
	ssize_t ret;
	FILE_EVENT *fe;

	if (sockfd == INVALID_SOCKET) {
		msg_error("%s: invalid sockfd: %d", __FUNCTION__, sockfd);
		return -1;
	}

	if (__sys_recvfrom == NULL) {
		hook_init();
	}

	if (!var_hook_sys_api) {
		return __sys_recvfrom(sockfd, buf, len,
				flags, src_addr, addrlen);
	}

	fe = fiber_file_open(sockfd);
	fiber_wait_read(fe);

	ret = __sys_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
	if (ret >= 0) {
		return ret;
	}

	fiber_save_errno();

	if (acl_fiber_killed(fe->fiber)) {
		msg_info("%s(%d), %s: fiber-%u is existing", __FILE__,
			__LINE__, __FUNCTION__, acl_fiber_id(fe->fiber));
	}

	return ret;
}

# ifdef SYS_UNIX
ssize_t fiber_recvmsg(socket_t sockfd, struct msghdr *msg, int flags)
{
	ssize_t ret;
	FILE_EVENT *fe;

	if (sockfd == INVALID_SOCKET) {
		msg_error("%s: invalid sockfd: %d", __FUNCTION__, sockfd);
		return -1;
	}

	if (__sys_recvmsg == NULL) {
		hook_init();
	}

	if (!var_hook_sys_api) {
		return __sys_recvmsg(sockfd, msg, flags);
	}

	fe = fiber_file_open(sockfd);
	fiber_wait_read(fe);

	ret = __sys_recvmsg(sockfd, msg, flags);
	if (ret >= 0) {
		return ret;
	}

	fiber_save_errno();

	if (acl_fiber_killed(fe->fiber)) {
		msg_info("%s(%d), %s: fiber-%u is existing", __FILE__,
			__LINE__, __FUNCTION__, acl_fiber_id(fe->fiber));
	}

	return ret;
}
# endif

#else

# ifdef SYS_UNIX
ssize_t fiber_read(socket_t fd, void *buf, size_t count)
{
	FILE_EVENT *fe;

	if (__sys_read == NULL) {
		hook_init();
	}

	while (1) {
		ssize_t n = __sys_read(fd, buf, count);

		if (!var_hook_sys_api) {
			return n;
		}

		if (n >= 0) {
			return n;
		}

		fiber_save_errno();

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN) {
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
			return -1;
		}

		fe = fiber_file_open(fd);
		fiber_wait_read(fe);

		if (acl_fiber_killed(fe->fiber)) {
			msg_info("%s(%d), %s: fiber-%u is existing",
				__FILE__, __LINE__, __FUNCTION__,
				acl_fiber_id(fe->fiber));
		}
	}
}

ssize_t fiber_readv(socket_t fd, const struct iovec *iov, int iovcnt)
{
	FILE_EVENT *fe;

	if (__sys_readv == NULL) {
		hook_init();
	}

	while (1) {
		ssize_t n = __sys_readv(fd, iov, iovcnt);

		if (!var_hook_sys_api) {
			return n;
		}

		if (n >= 0) {
			return n;
		}

		fiber_save_errno();

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN) {
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
			return -1;
		}

		fe = fiber_file_open(fd);
		fiber_wait_read(fe);

		if (acl_fiber_killed(fe->fiber)) {
			msg_info("%s(%d), %s: fiber-%u is existing",
				__FILE__, __LINE__, __FUNCTION__,
				acl_fiber_id(fe->fiber));
		}
	}
}
# endif

ssize_t fiber_recv(socket_t sockfd, void *buf, size_t len, int flags)
{
	FILE_EVENT *fe;

	if (__sys_recv == NULL) {
		hook_init();
	}

	while (1) {
		ssize_t n = __sys_recv(sockfd, buf, len, flags);

		if (!var_hook_sys_api) {
			return n;
		}

		if (n >= 0) {
			return n;
		}

		fiber_save_errno();

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN) {
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
			return -1;
		}

		fe = fiber_file_open(sockfd);
		fiber_wait_read(fe);

		if (acl_fiber_killed(fe->fiber)) {
			msg_info("%s(%d), %s: fiber-%u is existing",
				__FILE__, __LINE__, __FUNCTION__,
				acl_fiber_id(fe->fiber));
		}
	}
}

ssize_t fiber_recvfrom(socket_t sockfd, void *buf, size_t len, int flags,
	struct sockaddr *src_addr, socklen_t *addrlen)
{
	FILE_EVENT *fe;

	if (__sys_recvfrom == NULL) {
		hook_init();
	}

	while (1) {
		ssize_t n = __sys_recvfrom(sockfd, buf, len, flags,
				src_addr, addrlen);

		if (!var_hook_sys_api) {
			return n;
		}

		if (n >= 0) {
			return n;
		}

		fiber_save_errno();

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN) {
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
			return -1;
		}

		fe = fiber_file_open(sockfd);
		fiber_wait_read(fe);

		if (acl_fiber_killed(fe->fiber)) {
			msg_info("%s(%d), %s: fiber-%u is existing",
				__FILE__, __LINE__, __FUNCTION__,
				acl_fiber_id(fe->fiber));
		}
	}
}

# ifdef SYS_UNIX
ssize_t fiber_recvmsg(socket_t sockfd, struct msghdr *msg, int flags)
{
	FILE_EVENT *fe;

	if (__sys_recvmsg == NULL) {
		hook_init();
	}

	while (1) {
		ssize_t n = __sys_recvmsg(sockfd, msg, flags);

		if (!var_hook_sys_api) {
			return n;
		}

		if (n >= 0) {
			return n;
		}

		fiber_save_errno();

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN) {
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
			return -1;
		}

		fe = fiber_file_open(sockfd);
		fiber_wait_read(fe);

		if (acl_fiber_killed(fe->fiber)) {
			msg_info("%s(%d), %s: fiber-%u is existing",
				__FILE__, __LINE__, __FUNCTION__,
				acl_fiber_id(fe->fiber));
		}
	}
}
# endif
#endif  /* READ_WAIT_FIRST */

/****************************************************************************/

#ifdef SYS_UNIX
ssize_t fiber_write(socket_t fd, const void *buf, size_t count)
{
	FILE_EVENT *fe;

	if (__sys_write == NULL) {
		hook_init();
	}

	while (1) {
		ssize_t n = __sys_write(fd, buf, count);

		if (!var_hook_sys_api) {
			return n;
		}

		if (n >= 0) {
			return n;
		}

		fiber_save_errno();

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN) {
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
			return -1;
		}

		fe = fiber_file_open(fd);
		fiber_wait_write(fe);

		if (acl_fiber_killed(fe->fiber)) {
			msg_info("%s(%d), %s: fiber-%u is existing",
				__FILE__, __LINE__, __FUNCTION__,
				acl_fiber_id(fe->fiber));
			return -1;
		}
	}
}

ssize_t fiber_writev(socket_t fd, const struct iovec *iov, int iovcnt)
{
	FILE_EVENT *fe;

	if (__sys_writev == NULL) {
		hook_init();
	}

	while (1) {
		ssize_t n = __sys_writev(fd, iov, iovcnt);

		if (!var_hook_sys_api) {
			return n;
		}

		if (n >= 0) {
			return n;
		}

		fiber_save_errno();

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN) {
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
			return -1;
		}

		fe = fiber_file_open(fd);
		fiber_wait_write(fe);

		if (acl_fiber_killed(fe->fiber)) {
			msg_info("%s(%d), %s: fiber-%u is existing",
				__FILE__, __LINE__, __FUNCTION__,
				acl_fiber_id(fe->fiber));
			return -1;
		}
	}
}
#endif

ssize_t fiber_send(socket_t sockfd, const void *buf, size_t len, int flags)
{
	FILE_EVENT *fe;

	if (__sys_send == NULL) {
		hook_init();
	}

	while (1) {
		ssize_t n = __sys_send(sockfd, buf, len, flags);

		if (!var_hook_sys_api) {
			return n;
		}

		if (n >= 0) {
			return n;
		}

		fiber_save_errno();

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN) {
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
			return -1;
		}

		fe = fiber_file_open(sockfd);
		fiber_wait_write(fe);

		if (acl_fiber_killed(fe->fiber)) {
			msg_info("%s(%d), %s: fiber-%u is existing",
				__FILE__, __LINE__, __FUNCTION__,
				acl_fiber_id(fe->fiber));
			return -1;
		}
	}
}

ssize_t fiber_sendto(socket_t sockfd, const void *buf, size_t len, int flags,
	const struct sockaddr *dest_addr, socklen_t addrlen)
{
	FILE_EVENT *fe;

	if (__sys_sendto == NULL) {
		hook_init();
	}

	while (1) {
		ssize_t n = __sys_sendto(sockfd, buf, len, flags,
				dest_addr, addrlen);

		if (!var_hook_sys_api) {
			return n;
		}

		if (n >= 0) {
			return n;
		}

		fiber_save_errno();

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN) {
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
			return -1;
		}

		fe = fiber_file_open(sockfd);
		fiber_wait_write(fe);

		if (acl_fiber_killed(fe->fiber)) {
			msg_info("%s(%d), %s: fiber-%u is existing",
				__FILE__, __LINE__, __FUNCTION__,
				acl_fiber_id(fe->fiber));
			return -1;
		}
	}
}

#ifdef SYS_UNIX
ssize_t fiber_sendmsg(socket_t sockfd, const struct msghdr *msg, int flags)
{
	FILE_EVENT *fe;

	if (__sys_sendmsg == NULL) {
		hook_init();
	}

	while (1) {
		ssize_t n = __sys_sendmsg(sockfd, msg, flags);

		if (!var_hook_sys_api) {
			return n;
		}

		if (n >= 0) {
			return n;
		}

		fiber_save_errno();

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN) {
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
			return -1;
		}

		fe = fiber_file_open(sockfd);
		fiber_wait_write(fe);

		if (acl_fiber_killed(fe->fiber)) {
			msg_info("%s(%d), %s: fiber-%u is existing",
				__FILE__, __LINE__, __FUNCTION__,
				acl_fiber_id(fe->fiber));
			return -1;
		}
	}
}
#endif

/****************************************************************************/

#ifdef SYS_UNIX
ssize_t read(socket_t fd, void *buf, size_t count)
{
	return fiber_read(fd, buf, count);
}

ssize_t readv(socket_t fd, const struct iovec *iov, int iovcnt)
{
	return fiber_readv(fd, iov, iovcnt);
}
#endif

#ifdef ACL_ARM_LINUX

ssize_t recv(socket_t sockfd, void *buf, size_t len, unsigned int flags)
{
	return fiber_recv(sockfd, buf, len, (int) flags);
}

ssize_t recvfrom(socket_t sockfd, void *buf, size_t len, unsigned int flags,
	const struct sockaddr *src_addr, socklen_t *addrlen)
{
	return fiber_recvfrom(sockfd, buf, len, flags,
			(const struct sockaddr*) src_addr, addrlen);
}

ssize_t recvmsg(socket_t sockfd, struct msghdr *msg, unsigned int flags)
{
	return fiber_recvmsg(sockfd, msg, flags);
}

#elif defined(SYS_UNIX)

ssize_t recv(socket_t sockfd, void *buf, size_t len, int flags)
{
	return fiber_recv(sockfd, buf, len, (int) flags);
}

ssize_t recvfrom(socket_t sockfd, void *buf, size_t len, int flags,
	struct sockaddr *src_addr, socklen_t *addrlen)
{
	return fiber_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(socket_t sockfd, struct msghdr *msg, int flags)
{
	return fiber_recvmsg(sockfd, msg, flags);
}

#endif // SYS_UNIX

#ifdef SYS_UNIX
ssize_t write(socket_t fd, const void *buf, size_t count)
{
	return fiber_write(fd, buf, count);
}

ssize_t writev(socket_t fd, const struct iovec *iov, int iovcnt)
{
	return fiber_writev(fd, iov, iovcnt);
}
#endif

#ifdef ACL_ARM_LINUX

ssize_t send(socket_t sockfd, const void *buf, size_t len, unsigned int flags)
{
	return fiber_send(sockfd, buf, len, (int) flags);
}

ssize_t sendto(socket_t sockfd, const void *buf, size_t len, int flags,
	const struct sockaddr *dest_addr, socklen_t addrlen)
{
	return fiber_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

ssize_t sendmsg(socket_t sockfd, const struct msghdr *msg, unsigned int flags)
{
	return fiber_sendmsg(sockfd, msg, (int) flags);
}

#elif defined(SYS_UNIX)

ssize_t send(socket_t sockfd, const void *buf, size_t len, int flags)
{
	return fiber_send(sockfd, buf, len, flags);
}

ssize_t sendto(socket_t sockfd, const void *buf, size_t len, int flags,
	const struct sockaddr *dest_addr, socklen_t addrlen)
{
	return fiber_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

ssize_t sendmsg(socket_t sockfd, const struct msghdr *msg, int flags)
{
	return fiber_sendmsg(sockfd, msg, flags);
}

#endif

/****************************************************************************/

#ifdef  __USE_LARGEFILE64
ssize_t sendfile64(socket_t out_fd, int in_fd, off64_t *offset, size_t count)
{
	FILE_EVENT *fe;

	if (__sys_sendfile64 == NULL) {
		hook_init();
	}

	while (1) {
		ssize_t n = __sys_sendfile64(out_fd, in_fd, offset, count);
		if (!var_hook_sys_api || n >= 0) {
			return n;
		}

		fiber_save_errno();

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN) {
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
			return -1;
		}

		fe = fiber_file_open(out_fd);
		fiber_wait_write(fe);

		if (acl_fiber_killed(fe->fiber)) {
			msg_info("%s(%d), %s: fiber-%u is existing",
				__FILE__, __LINE__, __FUNCTION__,
				acl_fiber_id(fe->fiber));
			return -1;
		}
	}
}
#endif
