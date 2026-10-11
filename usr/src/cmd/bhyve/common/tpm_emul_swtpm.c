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
#include <sys/endian.h>
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

/*
 * swtpm control channel protocol, see swtpm's tpm_ioctl.h.  Every request
 * starts with the command code, all fields are big endian.
 */
#define	SWTPM_CMD_INIT		2

/* TPM2_Startup(TPM_SU_CLEAR) */
static const uint8_t tpm2_startup_clear[] = {
	0x80, 0x01,			/* TPM_ST_NO_SESSIONS */
	0x00, 0x00, 0x00, 0x0c,		/* commandSize */
	0x00, 0x00, 0x01, 0x44,		/* TPM_CC_Startup */
	0x00, 0x00			/* TPM_SU_CLEAR */
};
#define	TPM2_RC_SUCCESS		0x000
#define	TPM2_RC_INITIALIZE	0x100

static int tpm_swtpm_execute_cmd(void *, void *, uint32_t, void *, uint32_t);

struct swtpm_ctrl_init_req {
	uint32_t cmd;
	uint32_t init_flags;
} __packed;

static int
tpm_swtpm_connect(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	fd = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		warn("%s: unable to open socket", __func__);
		return (-1);
	}

	bzero(&addr, sizeof (addr));
	addr.sun_family = AF_UNIX;
	if (strlcpy(addr.sun_path, path, sizeof (addr.sun_path)) >=
	    sizeof (addr.sun_path)) {
		warnx("%s: socket path \"%s\" is too long", __func__, path);
		(void) close(fd);
		return (-1);
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof (addr)) == -1) {
		warn("%s: unable to connect to socket \"%s\"", __func__, path);
		(void) close(fd);
		return (-1);
	}

	return (fd);
}

/*
 * Initialize the TPM through swtpm's control channel, as a power-on of the
 * platform does.  Without this, a TPM that was already running, e.g. before a
 * guest reboot, keeps its state and PCR values, and swtpm has to be started
 * with --flags not-need-init.
 */
static int
tpm_swtpm_ctrl_init(const char *path)
{
	struct swtpm_ctrl_init_req req;
	uint8_t *p;
	uint32_t res;
	size_t done;
	ssize_t len;
	int fd, error = 0;

	fd = tpm_swtpm_connect(path);
	if (fd < 0)
		return (ENOENT);

	req.cmd = htobe32(SWTPM_CMD_INIT);
	req.init_flags = htobe32(0);

	for (p = (uint8_t *)&req, done = 0; done < sizeof (req); ) {
		len = send(fd, p + done, sizeof (req) - done, MSG_NOSIGNAL);
		if (len == -1) {
			if (errno == EINTR)
				continue;
			warn("%s: unable to send CMD_INIT", __func__);
			error = EIO;
			goto out;
		}
		done += len;
	}

	for (p = (uint8_t *)&res, done = 0; done < sizeof (res); ) {
		len = recv(fd, p + done, sizeof (res) - done, 0);
		if (len == -1) {
			if (errno == EINTR)
				continue;
			warn("%s: unable to receive CMD_INIT result", __func__);
			error = EIO;
			goto out;
		}
		if (len == 0) {
			warnx("%s: swtpm closed the control channel",
			    __func__);
			error = EIO;
			goto out;
		}
		done += len;
	}

	if (be32toh(res) != 0) {
		warnx("%s: CMD_INIT failed with TPM result 0x%x", __func__,
		    be32toh(res));
		error = EIO;
	}

out:
	(void) close(fd);
	return (error);
}

static int
tpm_swtpm_init(void **sc, nvlist_t *nvl)
{
	struct tpm_swtpm *tpm;
	const char *path, *ctrl;
	int error;

	path = get_config_value_node(nvl, "path");
	if (path == NULL) {
		warnx("%s: no socket path specified", __func__);
		return (ENOENT);
	}

	/*
	 * The control channel is optional.  If it is configured, initialize
	 * the TPM before the guest can use it.
	 */
	ctrl = get_config_value_node(nvl, "ctrl");
	if (ctrl != NULL) {
		error = tpm_swtpm_ctrl_init(ctrl);
		if (error != 0)
			return (error);
	}

	tpm = calloc(1, sizeof (struct tpm_swtpm));
	if (tpm == NULL) {
		warnx("%s: failed to allocate tpm_swtpm", __func__);
		return (ENOMEM);
	}

	tpm->fd = tpm_swtpm_connect(path);
	if (tpm->fd < 0) {
		free(tpm);
		return (ENOENT);
	}

	/*
	 * After CMD_INIT the TPM waits for TPM2_Startup, which is normally
	 * sent by TPM-aware firmware.  The bhyve UEFI firmware does not
	 * necessarily do this, so send it here.  Firmware that does send it
	 * gets TPM_RC_INITIALIZE, which it treats as "already started".
	 */
	if (ctrl != NULL) {
		uint8_t rsp[sizeof (struct tpm_resp_hdr)];
		const struct tpm_resp_hdr *hdr = (void *)rsp;
		uint32_t rc;

		error = tpm_swtpm_execute_cmd(tpm, (void *)tpm2_startup_clear,
		    sizeof (tpm2_startup_clear), rsp, sizeof (rsp));
		rc = be32toh(hdr->errcode);
		if (error == 0 && rc != TPM2_RC_SUCCESS &&
		    rc != TPM2_RC_INITIALIZE) {
			warnx("%s: TPM2_Startup failed with TPM result 0x%x",
			    __func__, rc);
			error = EIO;
		}
		if (error != 0) {
			(void) close(tpm->fd);
			free(tpm);
			return (error);
		}
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
