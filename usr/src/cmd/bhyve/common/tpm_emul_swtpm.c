/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 Hans Rosenfeld
 * Author: Hans Rosenfeld <rosenfeld@grumpf.hope-2000.org>
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#ifdef	__FreeBSD__
#include <malloc_np.h>
#else
#include <sys/vmm.h>
#include <endian.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "config.h"
#include "tpm_device.h"
#include "tpm_emul.h"

struct tpm_swtpm {
	int fd;
};

struct tpm_resp_hdr {
	uint16_t tag;
	uint32_t len;
	uint32_t errcode;
} __packed;

static int
tpm_swtpm_init(void **sc, nvlist_t *nvl)
{
	struct tpm_swtpm *tpm;
	const char *path;
	struct sockaddr_un tpm_addr;

	tpm = calloc(1, sizeof (struct tpm_swtpm));
	if (tpm == NULL) {
		warnx("%s: failed to allocate tpm_swtpm", __func__);
		return (ENOMEM);
	}

	path = get_config_value_node(nvl, "path");
	if (path == NULL) {
		warnx("%s: no socket path specified", __func__);
		return (ENOENT);
	}

	tpm->fd = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (tpm->fd < 0) {
		warnx("%s: unable to open tpm socket", __func__);
		return (ENOENT);
	}

	bzero(&tpm_addr, sizeof (tpm_addr));
	tpm_addr.sun_family = AF_UNIX;
	strlcpy(tpm_addr.sun_path, path, sizeof (tpm_addr.sun_path) - 1);

	if (connect(tpm->fd, (struct sockaddr *)&tpm_addr, sizeof (tpm_addr)) ==
	    -1) {
		warnx("%s: unable to connect to tpm socket \"%s\"", __func__,
		    path);
		return (ENOENT);
	}

	*sc = tpm;

	return (0);
}

static int
tpm_swtpm_execute_cmd(void *sc, void *cmd, uint32_t cmd_size, void *rsp,
    uint32_t rsp_size)
{
	struct tpm_swtpm *tpm;
	ssize_t len;

	if (rsp_size < (ssize_t)sizeof(struct tpm_resp_hdr)) {
		warn("%s: rsp_size of %u is too small", __func__, rsp_size);
		return (EINVAL);
	}

	tpm = sc;

#ifdef	__FreeBSD__
	len = send(tpm->fd, cmd, cmd_size, MSG_NOSIGNAL|MSG_DONTWAIT);
	if (len == -1)
		err(1, "%s: cmd send failed, is swtpm running?", __func__);
	if (len != cmd_size) {
		warn("%s: cmd write failed (bytes written: %zd / %d)", __func__,
		    len, cmd_size);
		return (EFAULT);
	}

	len = recv(tpm->fd, rsp, rsp_size, 0);
	if (len == -1)
		err(1, "%s: rsp recv failed, is swtpm running?", __func__);
	if (len < (ssize_t)sizeof(struct tpm_resp_hdr)) {
		warn("%s: rsp read failed (bytes read: %zd / %d)", __func__,
		    len, rsp_size);
		return (EFAULT);
	}
#else
	while (cmd_size > 0) {
		len = send(tpm->fd, cmd, cmd_size, MSG_NOSIGNAL|MSG_DONTWAIT);
		if (len == -1) {
			if (errno == EINTR)
				continue;
			err(1, "%s: cmd send failed, is swtpm running?",
			    __func__);
		}
		cmd += len;
		cmd_size -= len;
	}

	/*
	 * The response can arrive in several pieces.  Read the header first
	 * to learn the response length, then read until the whole response
	 * is in.  Stopping early would leave the rest in the socket, where it
	 * would be mistaken for the start of the next response.
	 */
	uint8_t *buf = rsp;
	size_t rcvd = 0;
	size_t want = sizeof (struct tpm_resp_hdr);
	size_t total = 0;

	while (rcvd < want) {
		len = recv(tpm->fd, buf + rcvd, want - rcvd, 0);
		if (len == -1) {
			if (errno == EINTR)
				continue;
			err(1, "%s: rsp recv failed, is swtpm running?",
			    __func__);
		}
		if (len == 0) {
			warnx("%s: swtpm closed the connection (bytes read: "
			    "%zu / %zu)", __func__, rcvd, want);
			return (EFAULT);
		}
		rcvd += len;

		if (total == 0 && rcvd >= sizeof (struct tpm_resp_hdr)) {
			const struct tpm_resp_hdr *hdr = rsp;

			total = be32toh(hdr->len);
			if (total < sizeof (struct tpm_resp_hdr)) {
				warnx("%s: invalid response length %zu",
				    __func__, total);
				return (EFAULT);
			}
			want = MIN(total, rsp_size);
		}
	}

	/*
	 * A response larger than the guest's buffer cannot be returned in
	 * full.  Drain the remainder to keep the socket in sync.
	 */
	while (rcvd < total) {
		uint8_t discard[256];

		len = recv(tpm->fd, discard,
		    MIN(sizeof (discard), total - rcvd), 0);
		if (len == -1) {
			if (errno == EINTR)
				continue;
			err(1, "%s: rsp recv failed, is swtpm running?",
			    __func__);
		}
		if (len == 0)
			break;
		rcvd += len;
	}
	if (total > rsp_size) {
		warnx("%s: response of %zu bytes truncated to %u", __func__,
		    total, rsp_size);
		return (EFAULT);
	}
#endif

	return (0);
}

static void
tpm_swtpm_deinit(void *sc)
{
	struct tpm_swtpm *tpm;

	tpm = sc;
	if (tpm == NULL)
		return;

	if (tpm->fd >= 0)
		close(tpm->fd);

	free(tpm);
}

static const struct tpm_emul tpm_emul_swtpm = {
	.name = "swtpm",
	.init = tpm_swtpm_init,
	.deinit = tpm_swtpm_deinit,
	.execute_cmd = tpm_swtpm_execute_cmd,
};
TPM_EMUL_SET(tpm_emul_swtpm);
