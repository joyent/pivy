/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2018, Joyent Inc
 * Author: Alex Wilson <alex.wilson@joyent.com>
 */

#if !defined(_EBOX_H)
#define _EBOX_H

#include <stdint.h>
#include <assert.h>

#if defined(__APPLE__)
#include <PCSC/wintypes.h>
#include <PCSC/winscard.h>
#else
#include <wintypes.h>
#include <winscard.h>
#endif

#include <sys/types.h>
#include <sys/uio.h>

#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "errf.h"
#include "piv.h"
#include "libssh/digest.h"

enum ebox_type {
	EBOX_TEMPLATE = 0x01,
	EBOX_KEY = 0x02,
	EBOX_STREAM = 0x03
};

enum ebox_recov_tag {
	EBOX_RECOV_TOKEN = 0x01,
	EBOX_RECOV_KEY = 0x02
};

enum ebox_config_type {
	EBOX_PRIMARY = 0x01,
	EBOX_RECOVERY = 0x02
};

enum ebox_stream_mode {
	EBOX_MODE_ENCRYPT = 0x01,
	EBOX_MODE_DECRYPT = 0x02
};

enum ebox_part_tag {
	EBOX_PART_END = 0,
	EBOX_PART_PUBKEY = 1,
	EBOX_PART_NAME = 2,
	EBOX_PART_CAK = 3,
	EBOX_PART_GUID = 4,
	EBOX_PART_BOX = 5
};

void ebox_tpl_free(struct ebox_tpl *tpl);
void ebox_tpl_config_free(struct ebox_tpl_config *config);
void ebox_tpl_part_free(struct ebox_tpl_part *part);

struct ebox_tpl *ebox_tpl_clone(struct ebox_tpl *tpl);

int sshbuf_get_ebox_tpl(struct sshbuf *buf, struct ebox_tpl **tpl);
int sshbuf_put_ebox_tpl(struct sshbuf *buf, struct ebox_tpl *tpl);

void ebox_free(struct ebox *box);
void ebox_config_free(struct ebox_config *config);
void ebox_part_free(struct ebox_part *part);

int sshbuf_get_ebox(struct sshbuf *buf, struct ebox **box);
int sshbuf_put_ebox(struct sshbuf *buf, struct ebox *box);

void ebox_stream_free(struct ebox_stream *str);
void ebox_stream_chunk_free(struct ebox_stream_chunk *chunk);

/*
 * Creates a new ebox based on a given template, sealing up the provided key
 * and (optional) recovery token.
 */
struct ebox *ebox_create(const struct ebox_tpl *tpl, const uint8_t *key,
    size_t keylen, const uint8_t *token, size_t tokenlen);

/*
 * Generate a challenge for a given recovery config + part.
 *
 * The challenge can then be serialised using sshbuf_put_ebox_challenge() and
 * sent to the remote side. The "descfmt", ... arguments are given to vsnprintf
 * to create the "description" field for the challenge (displayed on the
 * remote end).
 *
 * Errors:
 *  - ENOMEM: description was too long for available space
 */
int ebox_gen_challenge(struct ebox_config *config, struct ebox_part *part,
    const char *descfmt, ...);

void ebox_challenge_free(struct ebox_challenge *chal);

/*
 * Serializes an ebox challenge inside a piv_ecdh_box as a one-step process.
 *
 * The data written in the buf is ready to be transported to a remote machine.
 */
int sshbuf_put_ebox_challenge(struct sshbuf *buf, struct ebox_challenge *chal);

/*
 * De-serializes an ebox challenge from inside a piv_ecdh_box. The piv_ecdh_box
 * must be already unsealed.
 */
int sshbuf_get_ebox_challenge(struct piv_ecdh_box *box,
    struct ebox_challenge **chal);

/*
 * Serializes a response to an ebox challenge inside a piv_ecdh_box as a
 * one-step process. The c_keybox on chal must be already unsealed.
 *
 * The data written in the buf is ready to be transported to the original
 * requesting machine.
 */
int sshbuf_put_ebox_challenge_response(struct sshbuf *buf,
    struct ebox_challenge *chal);

/*
 * Process an incoming response to a recovery challenge for the given config.
 *
 * *ppart is set to point at the part that this response was from. Takes
 * ownership of respbox, and will free it.
 *
 * Errors:
 *  - EAGAIN: this challenge matched a part that is already unlocked
 */
int ebox_challenge_response(struct ebox_config *config,
    struct piv_ecdh_box *respbox, struct ebox_part **ppart);

/*
 * Unlock an ebox using a primary config.
 *
 * One of the primary config's part boxes must have been already unsealed
 * before calling this.
 *
 * Errors:
 *  - EINVAL: none of the part boxes were unsealed
 */
int ebox_unlock(struct ebox *ebox, struct ebox_config *config);

/*
 * Perform recovery on an ebox using a recovery config.
 *
 * N out of M of the parts on this config must have been processed with
 * ebox_challenge_response() before calling this.
 *
 * Errors:
 *  - EINVAL: insufficient number of parts available on this config that are
 *            ready for recovery
 *  - EAGAIN: the ebox is already unlocked or recovered
 *  - EBADF: the recovery box data was invalid or corrupt
 */
int ebox_recover(struct ebox *ebox, struct ebox_config *config);

int sshbuf_get_ebox_stream(struct sshbuf *buf, struct ebox_stream **str);
int sshbuf_put_ebox_stream(struct sshbuf *buf, struct ebox_stream *str);
int sshbuf_get_ebox_stream_chunk(struct sshbuf *buf,
    struct ebox_stream_chunk **chunk);
int sshbuf_put_ebox_stream_chunk(struct sshbuf *buf,
    struct ebox_stream_chunk *chunk);

struct ebox_stream *ebox_stream_init_decrypt(void);
struct ebox_stream *ebox_stream_init_encrypt(struct ebox_tpl *tpl);
int ebox_stream_put(struct ebox_stream *str, struct iovec *vecs, size_t nvecs);
int ebox_stream_get(struct ebox_stream *str, struct iovec *vecs, size_t nvecs);

#endif
