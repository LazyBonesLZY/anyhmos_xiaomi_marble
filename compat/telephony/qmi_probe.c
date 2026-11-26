/* SPDX-License-Identifier: MIT
 *
 * Read-only QRTR endpoint and QMI request-result diagnostic for marble.
 * QRTR addresses come from the local nameserver, not from GLINK/DTS IDs.
 * A listing is a snapshot of advertisements, not proof of MSS identity,
 * SIM readiness, registration, signal, or other telephony business results.
 *
 * Uses the Linux QRTR UAPI without libqrtr, libqmi or vendor libraries.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <linux/qrtr.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#define QMI_MAX_MSG 4096
#define QMI_HDR_LEN 7
#define QMI_MSG_REQUEST 0x00
#define QMI_MSG_RESPONSE 0x02
#define QMI_TLV_RESULT 0x02

#define QMI_SVC_NAS 0x03
#define QMI_SVC_UIM 0x0b
#define QMI_UIM_GET_CARD_STATUS 0x002f
#define QMI_NAS_GET_SERVING_SYSTEM 0x0024

#define MAX_ENDPOINTS 32
#define QRTR_FIELD(field) offsetof(struct qrtr_ctrl_pkt, field)

struct endpoint {
	uint32_t node;
	uint32_t port;
	uint32_t instance;
};

struct snapshot {
	struct endpoint entries[MAX_ENDPOINTS];
	size_t count;
	bool complete;
};

struct client {
	int fd;
	struct sockaddr_qrtr local;
};

struct sender {
	struct sockaddr_qrtr addr;
	socklen_t len;
	int flags;
};

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static int fail(int error)
{
	errno = error;
	return -1;
}

static int64_t monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return -1;
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_readable(int fd, int64_t deadline)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	for (;;) {
		int64_t now = monotonic_ms();
		int rc;

		if (now < 0)
			return -1;
		if (now >= deadline)
			return fail(ETIMEDOUT);
		rc = poll(&pfd, 1, (int)(deadline - now));
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0)
			return -1;
		if (rc == 0)
			return fail(ETIMEDOUT);
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return fail(EIO);
		if (pfd.revents & POLLIN)
			return 0;
	}
}

static int qrtr_open(struct client *c)
{
	socklen_t len = sizeof(c->local);
	int saved;

	c->fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (c->fd < 0)
		return -1;
	if (getsockname(c->fd, (struct sockaddr *)&c->local, &len) < 0)
		goto error;
	if (len != sizeof(c->local) || c->local.sq_family != AF_QIPCRTR ||
	    c->local.sq_node == QRTR_NODE_BCAST) {
		errno = EPROTO;
		goto error;
	}
	c->local.sq_port = 0; /* bind an ephemeral local port, not the control port */
	if (bind(c->fd, (struct sockaddr *)&c->local, sizeof(c->local)) < 0)
		goto error;
	return 0;

error:
	saved = errno;
	close(c->fd);
	return fail(saved);
}

static int qrtr_send(const struct client *c, uint32_t node, uint32_t port,
		     const void *buf, size_t size)
{
	struct sockaddr_qrtr dst = {
		.sq_family = AF_QIPCRTR, .sq_node = node, .sq_port = port,
	};
	ssize_t n = sendto(c->fd, buf, size, MSG_DONTWAIT,
			   (struct sockaddr *)&dst, sizeof(dst));

	if (n < 0)
		return -1;
	return (size_t)n == size ? 0 : fail(EIO);
}

static int qrtr_lookup(const struct client *c, uint32_t cmd, uint32_t service)
{
	uint8_t buf[sizeof(struct qrtr_ctrl_pkt)] = {0};

	put32(buf + QRTR_FIELD(cmd), cmd);
	put32(buf + QRTR_FIELD(server.service), service);
	/* A zero instance is the nameserver wildcard, not a guessed version. */
	return qrtr_send(c, c->local.sq_node, QRTR_PORT_CTRL, buf, sizeof(buf));
}

static ssize_t qrtr_recv(const struct client *c, void *buf, size_t size,
			 struct sender *from)
{
	struct iovec iov = { .iov_base = buf, .iov_len = size };
	struct msghdr msg = {
		.msg_name = &from->addr, .msg_namelen = sizeof(from->addr),
		.msg_iov = &iov, .msg_iovlen = 1,
	};
	ssize_t n = recvmsg(c->fd, &msg, MSG_DONTWAIT);

	from->len = msg.msg_namelen;
	from->flags = msg.msg_flags;
	return n;
}

static bool from_endpoint(const struct sender *from, uint32_t node, uint32_t port)
{
	return from->len == sizeof(from->addr) && from->addr.sq_family == AF_QIPCRTR &&
	       from->addr.sq_node == node && from->addr.sq_port == port;
}

/* Only the local nameserver can complete a listing.  Read its payload address,
 * not the control datagram's sender address, to obtain the advertised peer.
 */
static int apply_notice(const struct client *c, struct snapshot *s, uint32_t service,
			const struct sender *from, const uint8_t *buf, size_t size)
{
	struct endpoint peer;
	uint32_t cmd, id;
	size_t i;

	if (!from_endpoint(from, c->local.sq_node, QRTR_PORT_CTRL))
		return 0;
	if (from->flags & MSG_TRUNC || size != sizeof(struct qrtr_ctrl_pkt))
		return fail(EPROTO);
	cmd = le32(buf + QRTR_FIELD(cmd));
	if (cmd != QRTR_TYPE_NEW_SERVER && cmd != QRTR_TYPE_DEL_SERVER)
		return 0;
	id = le32(buf + QRTR_FIELD(server.service));
	peer.instance = le32(buf + QRTR_FIELD(server.instance));
	peer.node = le32(buf + QRTR_FIELD(server.node));
	peer.port = le32(buf + QRTR_FIELD(server.port));

	/* Local ns.c lookup_notify(..., NULL, true): NEW_SERVER with zero fields. */
	if (!id && !peer.instance && !peer.node && !peer.port) {
		if (cmd == QRTR_TYPE_NEW_SERVER)
			s->complete = true;
		return 0;
	}
	if (id != service)
		return 0;
	if (peer.node == QRTR_NODE_BCAST || !peer.port || peer.port == QRTR_PORT_CTRL)
		return fail(EPROTO);

	for (i = 0; i < s->count; i++) {
		struct endpoint *entry = &s->entries[i];

		if (entry->node != peer.node || entry->port != peer.port)
			continue;
		if (cmd == QRTR_TYPE_DEL_SERVER)
			*entry = s->entries[--s->count];
		else if (entry->instance != peer.instance)
			return fail(EPROTO); /* conflicting advertisements for the same address */
		return 0; /* repeated identical advertisement */
	}
	if (cmd == QRTR_TYPE_DEL_SERVER)
		return 0;
	if (s->count == MAX_ENDPOINTS)
		return fail(EOVERFLOW);
	s->entries[s->count++] = peer;
	return 0;
}

static int collect_snapshot(const struct client *c, struct snapshot *s, uint32_t service)
{
	int64_t start = monotonic_ms();
	int64_t deadline = start + 900;

	if (start < 0)
		return -1;
	for (;;) {
		struct sender from = {0};
		uint8_t buf[sizeof(struct qrtr_ctrl_pkt)];
		int64_t now = monotonic_ms();
		ssize_t n;

		if (now < 0)
			return -1;
		if (now >= deadline)
			return fail(ETIMEDOUT);
		if (!s->complete && wait_readable(c->fd, deadline) < 0)
			return -1;
		n = qrtr_recv(c, buf, sizeof(buf), &from);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (s->complete)
					return 0; /* marker and all already-queued updates consumed */
				continue;
			}
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (apply_notice(c, s, service, &from, buf, (size_t)n) < 0)
			return -1;
	}
}

/* This common TLV is only a QMI request result.  Without a vetted service
 * schema, leave the remaining body opaque rather than claiming SIM or RAT.
 */
static int qmi_result(const uint8_t *buf, size_t size, uint16_t *error)
{
	size_t pos = QMI_HDR_LEN;
	bool seen = false;

	if (size < QMI_HDR_LEN || (size_t)le16(buf + 5) != size - QMI_HDR_LEN)
		return fail(EPROTO);
	while (pos < size) {
		uint16_t len;

		if (size - pos < 3)
			return fail(EPROTO);
		len = le16(buf + pos + 1);
		if (len > size - pos - 3)
			return fail(EPROTO);
		if (buf[pos] == QMI_TLV_RESULT) {
			uint16_t result;

			if (seen || len != 4)
				return fail(EPROTO);
			result = le16(buf + pos + 3);
			*error = le16(buf + pos + 5);
			if (result > 1 || (result == 0) != (*error == 0))
				return fail(EPROTO);
			seen = true;
		}
		pos += (size_t)len + 3;
	}
	return seen ? 0 : fail(EPROTO);
}

static int qmi_exchange(const struct client *c, struct snapshot *s, uint32_t service,
			uint16_t msg_id, uint16_t *error)
{
	const struct endpoint peer = s->entries[0];
	const uint16_t txn = 1; /* one request on this socket */
	uint8_t req[QMI_HDR_LEN] = { QMI_MSG_REQUEST };
	int64_t start = monotonic_ms();
	int64_t deadline = start + 3000;

	if (start < 0)
		return -1;
	put16(req + 1, txn);
	put16(req + 3, msg_id);
	if (qrtr_send(c, peer.node, peer.port, req, sizeof(req)) < 0)
		return -1;

	for (;;) {
		struct sender from = {0};
		uint8_t resp[QMI_MAX_MSG];
		ssize_t n;

		if (wait_readable(c->fd, deadline) < 0)
			return -1;
		n = qrtr_recv(c, resp, sizeof(resp), &from);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return -1;
		}
		if (from_endpoint(&from, c->local.sq_node, QRTR_PORT_CTRL)) {
			if (apply_notice(c, s, service, &from, resp, (size_t)n) < 0)
				return -1;
			if (s->count != 1 || s->entries[0].node != peer.node ||
			    s->entries[0].port != peer.port || s->entries[0].instance != peer.instance)
				return fail(ESTALE); /* observed a change, do not retarget the query */
			continue;
		}
		if (!from_endpoint(&from, peer.node, peer.port) || n < QMI_HDR_LEN ||
		    resp[0] != QMI_MSG_RESPONSE || le16(resp + 1) != txn ||
		    le16(resp + 3) != msg_id)
			continue;
		if (from.flags & MSG_TRUNC)
			return fail(EPROTO);
		return qmi_result(resp, (size_t)n, error);
	}
}

/* Separate sockets prevent one service's notifications/EOF from completing a
 * different lookup.  Keep the subscription while a request is outstanding.
 */
static int probe_service(uint32_t service, const char *name, uint16_t request)
{
	struct client c = {0};
	struct snapshot s = {0};
	int rc = 1;

	if (qrtr_open(&c) < 0) {
		int err = errno;

		printf("QRTR_LOOKUP name=%s svc=0x%02x status=socket_error errno=%d\n",
		       name, service, err);
		return err == EAFNOSUPPORT || err == EPROTONOSUPPORT ? 2 : 1;
	}
	if (qrtr_lookup(&c, QRTR_TYPE_NEW_LOOKUP, service) < 0) {
		printf("QRTR_LOOKUP name=%s svc=0x%02x status=send_error errno=%d\n",
		       name, service, errno);
		goto out;
	}
	if (collect_snapshot(&c, &s, service) < 0) {
		printf("QRTR_LOOKUP name=%s svc=0x%02x status=incomplete errno=%d\n",
		       name, service, errno);
	} else if (!s.count) {
		printf("QRTR_LOOKUP name=%s svc=0x%02x status=not_listed\n", name, service);
	} else if (s.count != 1) {
		printf("QRTR_LOOKUP name=%s svc=0x%02x status=ambiguous count=%zu\n", name, service, s.count);
	} else if (s.entries[0].node == c.local.sq_node) {
		printf("QRTR_LOOKUP name=%s svc=0x%02x status=local_only\n", name, service);
	} else {
		const struct endpoint *e = &s.entries[0];

		printf("QRTR_LOOKUP name=%s svc=0x%02x status=unique_remote node=%u port=0x%x instance=0x%x\n",
		       name, service, e->node, e->port, e->instance);
		rc = 0;
		if (request) {
			uint16_t error;

			if (qmi_exchange(&c, &s, service, request, &error) < 0) {
				printf("QMI_QUERY name=%s status=unconfirmed errno=%d\n", name, errno);
				rc = 1;
			} else {
				printf("QMI_QUERY name=%s request_result=%s error=0x%04x\n", name,
				       error ? "failed" : "ok", error);
				rc = error ? 3 : 0;
			}
		}
	}
	(void)qrtr_lookup(&c, QRTR_TYPE_DEL_LOOKUP, service);
out:
	close(c.fd);
	return rc;
}

static int list_services(void)
{
	static const struct {
		uint32_t id;
		const char *name;
	} services[] = {
		{0x01, "wds"}, {0x02, "dms"}, {QMI_SVC_NAS, "nas"},
		{QMI_SVC_UIM, "uim"}, {0x24, "pdc"},
	};
	size_t i, unique = 0;

	for (i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
		int rc = probe_service(services[i].id, services[i].name, 0);

		if (rc == 2)
			return 2;
		unique += rc == 0;
	}
	printf("QRTR_UNIQUE_REMOTE_TOTAL=%zu\n", unique);
	return unique ? 0 : 1;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--services] [--sim-status] [--serving-system] [--all]\n"
		"Read-only QMI/QRTR probe: default lists advertisements; optional UIM/NAS\n"
		"GET modes report request results only, not identity or business payloads.\n"
		"Listing succeeds if at least one service has a unique remote advertisement.\n"
		"Exit: 0 checks succeeded at their own layer, 1 unconfirmed, 2 usage/no QRTR,\n"
		"      3 QMI request rejected by peer.\n", argv0);
}

int main(int argc, char **argv)
{
	bool services = argc < 2, sim = false, nas = false;
	int rc = 0, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--services"))
			services = true;
		else if (!strcmp(argv[i], "--sim-status"))
			sim = true;
		else if (!strcmp(argv[i], "--serving-system"))
			nas = true;
		else if (!strcmp(argv[i], "--all"))
			services = sim = nas = true;
		else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	if (services)
		rc = list_services();
	if (sim) {
		int result = probe_service(QMI_SVC_UIM, "uim", QMI_UIM_GET_CARD_STATUS);

		if (result > rc)
			rc = result;
	}
	if (nas) {
		int result = probe_service(QMI_SVC_NAS, "nas", QMI_NAS_GET_SERVING_SYSTEM);

		if (result > rc)
			rc = result;
	}
	return rc;
}
