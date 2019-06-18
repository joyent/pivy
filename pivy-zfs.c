/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2019, Joyent Inc
 * Author: Alex Wilson <alex.wilson@joyent.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <strings.h>
#include <limits.h>
#include <err.h>

#if defined(__APPLE__)
#include <PCSC/wintypes.h>
#include <PCSC/winscard.h>
#else
#include <wintypes.h>
#include <winscard.h>
#endif

#include <sys/types.h>
#include <sys/errno.h>
#if defined(__sun)
#include <sys/fork.h>
#endif
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "libssh/sshkey.h"
#include "libssh/sshbuf.h"
#include "libssh/digest.h"
#include "libssh/ssherr.h"
#include "libssh/authfd.h"

#include "sss/hazmat.h"

#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#if defined(__sun)
#include <libtecla.h>
#else
#include <editline/readline.h>
#endif

#include <libzfs.h>
#include <libzfs_core.h>
#include <libnvpair.h>
#include <sys/dmu.h>

#define USING_SPL
#include "debug.h"
#include "tlv.h"
#include "errf.h"
#include "ebox.h"
#include "piv.h"
#include "bunyan.h"

#include "ebox-cmd.h"

static libzfs_handle_t *zfshdl = NULL;
static struct ebox_tpl *zfsebtpl = NULL;

static void usage(void);

static errf_t *
unlock_or_recover(struct ebox *ebox, const char *descr, boolean_t *recovered)
{
	struct ebox_config *config;
	struct ebox_part *part;
	struct ebox_tpl_part *tpart;
	struct ebox_tpl_config *tconfig;
	errf_t *error;
	struct question *q = NULL;
	struct answer *a;
	char k = '0';

	config = NULL;
	while ((config = ebox_next_config(ebox, config)) != NULL) {
		tconfig = ebox_config_tpl(config);
		if (ebox_tpl_config_type(tconfig) == EBOX_PRIMARY) {
			part = ebox_config_next_part(config, NULL);
			tpart = ebox_part_tpl(part);
			error = local_unlock(ebox_part_box(part),
			    ebox_tpl_part_cak(tpart),
			    ebox_tpl_part_name(tpart));
			if (error && !errf_caused_by(error, "NotFoundError"))
				return (error);
			if (error) {
				errf_free(error);
				continue;
			}
			error = ebox_unlock(ebox, config);
			if (error)
				return (error);
			*recovered = B_FALSE;
			goto done;
		}
	}

	q = calloc(1, sizeof (struct question));
	question_printf(q, "-- Recovery mode --\n");
	question_printf(q, "No primary configuration could proceed using a "
	    "token currently available\non the system. You may either select "
	    "a primary config to retry, or select\na recovery config to "
	    "begin the recovery process.\n\n");
	question_printf(q, "Select a configuration to use:");
	config = NULL;
	while ((config = ebox_next_config(ebox, config)) != NULL) {
		tconfig = ebox_config_tpl(config);
		a = ebox_config_alloc_private(config, sizeof (struct answer));
		a->a_key = ++k;
		a->a_priv = config;
		make_answer_text_for_config(tconfig, a);
		add_answer(q, a);
	}
again:
	question_prompt(q, &a);
	config = (struct ebox_config *)a->a_priv;
	tconfig = ebox_config_tpl(config);
	if (ebox_tpl_config_type(tconfig) == EBOX_PRIMARY) {
		part = ebox_config_next_part(config, NULL);
		tpart = ebox_part_tpl(part);
		error = local_unlock(ebox_part_box(part),
		    ebox_tpl_part_cak(tpart),
		    ebox_tpl_part_name(tpart));
		if (error) {
			warnfx(error, "failed to activate config %c", a->a_key);
			errf_free(error);
			goto again;
		}
		error = ebox_unlock(ebox, config);
		if (error)
			return (error);
		*recovered = B_FALSE;
		goto done;
	}
	error = interactive_recovery(config, descr);
	if (error) {
		warnfx(error, "failed to activate config %c", a->a_key);
		errf_free(error);
		goto again;
	}
	error = ebox_recover(ebox, config);
	if (error)
		return (error);

	*recovered = B_TRUE;

done:
	question_free(q);
	return (ERRF_OK);
}

static void
cmd_unlock(const char *fsname)
{
	zfs_handle_t *ds;
	nvlist_t *props, *prop;
	char *b64, *description;
	struct sshbuf *buf;
	struct ebox *ebox, *nebox;
	struct ebox_tpl *ntpl;
	struct ebox_tpl_config *tconfig;
	struct ebox_tpl_part *tpart;
	size_t desclen;
	errf_t *error;
	char *line;
	boolean_t recovered;
	int rc;
	const uint8_t *key;
	size_t keylen;
#if defined(DMU_OT_ENCRYPTED)
	uint64_t kstatus;
#endif

	ds = zfs_open(zfshdl, fsname, ZFS_TYPE_DATASET);
	if (ds == NULL)
		err(EXIT_ERROR, "failed to open dataset %s", fsname);

#if defined(DMU_OT_ENCRYPTED)
	props = zfs_get_all_props(ds);
	VERIFY(props != NULL);

	if (nvlist_lookup_nvlist(props, "keystatus", &prop)) {
		errx(EXIT_ERROR, "no keystatus property "
		    "could be read on dataset %s", fsname);
	}
	VERIFY0(nvlist_lookup_uint64(prop, "value", &kstatus));

	if (kstatus == ZFS_KEYSTATUS_AVAILABLE) {
		errx(EXIT_ALREADY_UNLOCKED, "key already loaded for %s",
		    fsname);
	}
#else
	props = zfs_get_user_props(ds);
	VERIFY(props != NULL);
#endif

	if (nvlist_lookup_nvlist(props, "rfd77:ebox", &prop)) {
		errx(EXIT_ERROR, "no rfd77:ebox property "
		    "could be read on dataset %s", fsname);
	}

	VERIFY0(nvlist_lookup_string(prop, "value", &b64));

	/* We use this string for the recovery flavour text. */
	desclen = strlen(fsname) + 128;
	description = calloc(1, desclen);
	snprintf(description, desclen, "ZFS filesystem %s", fsname);

	buf = sshbuf_new();
	if (buf == NULL)
		err(EXIT_ERROR, "failed to allocate buffer");
	if ((rc = sshbuf_b64tod(buf, b64))) {
		error = ssherrf("sshbuf_b64tod", rc);
		errfx(EXIT_ERROR, error, "failed to parse rfd77:ebox property"
		    " on %s as base64", fsname);
	}
	if ((error = sshbuf_get_ebox(buf, &ebox))) {
		errfx(EXIT_ERROR, error, "failed to parse rfd77:ebox property"
		    " on %s as a valid ebox", fsname);
	}

	fprintf(stderr, "Attempting to unlock ZFS '%s'...\n", fsname);
	(void) mlockall(MCL_CURRENT | MCL_FUTURE);
	if ((error = unlock_or_recover(ebox, description, &recovered)))
		errfx(EXIT_ERROR, error, "failed to unlock ebox");

	key = ebox_key(ebox, &keylen);
#if defined(MADV_DONTDUMP)
	(void) madvise((void *)key, keylen, MADV_DONTDUMP);
#endif
#if !defined(DMU_OT_ENCRYPTED)
	errx(EXIT_ERROR, "this ZFS implementation does not support encryption");
#else
	rc = lzc_load_key(fsname, B_FALSE, (uint8_t *)key, keylen);
	if (rc != 0) {
		errno = rc;
		err(EXIT_ERROR, "failed to load key material into ZFS for %s",
		    fsname);
	}
#endif

	if (recovered) {
		fprintf(stderr, "-- Add new primary configuration --\n");
		fprintf(stderr, "If the original primary PIV token has been "
		    "lost or damaged, it is recommended\nthat you add a new "
		    "primary token now. You can then use `pivy-zfs rekey' "
		    "later\nto remove the old primary device.\n\n");
ynagain:
		line = readline("Add new primary now? [Y/n] ");
		if (line == NULL)
			exit(EXIT_ERROR);
		if (line[0] == '\0' || (line[1] == '\0' &&
		    (line[0] == 'Y' || line[0] == 'y'))) {
			free(line);
			goto newprimary;
		} else if (line[1] == '\0' && (line[0] == 'n' || line[0] == 'N')) {
			free(line);
			goto done;
		} else {
			free(line);
			goto ynagain;
		}

newprimary:
		tpart = NULL;
		interactive_select_local_token(&tpart);
		if (tpart == NULL)
			goto done;
		tconfig = ebox_tpl_config_alloc(EBOX_PRIMARY);
		ebox_tpl_config_add_part(tconfig, tpart);

		ntpl = ebox_tpl_clone(ebox_tpl(ebox));
		ebox_tpl_add_config(ntpl, tconfig);

		error = ebox_create(ntpl, key, keylen, NULL, 0, &nebox);
		if (error)
			errfx(EXIT_ERROR, error, "ebox_create failed");
		sshbuf_reset(buf);
		error = sshbuf_put_ebox(buf, nebox);
		if (error)
			errfx(EXIT_ERROR, error, "sshbuf_put_ebox failed");

		b64 = sshbuf_dtob64(buf);

		rc = zfs_prop_set(ds, "rfd77:ebox", b64);
		if (rc != 0) {
			errno = rc;
			err(EXIT_ERROR, "failed to set ZFS property rfd77:ebox "
			    "on dataset %s", fsname);
		}

		free(b64);
		ebox_tpl_free(ntpl);
	}

done:
	ebox_free(ebox);
	sshbuf_free(buf);
	zfs_close(ds);
}

static void
cmd_rekey(const char *fsname)
{
	zfs_handle_t *ds;
	nvlist_t *props, *prop;
	char *b64, *description;
	struct sshbuf *buf;
	struct ebox *ebox, *nebox;
	size_t desclen;
	errf_t *error;
	boolean_t recovered;
	int rc;
	const uint8_t *key;
	size_t keylen;

	if (zfsebtpl == NULL) {
		warnx("-t <tplname|path> option is required");
		usage();
	}

	ds = zfs_open(zfshdl, fsname, ZFS_TYPE_DATASET);
	if (ds == NULL)
		err(EXIT_ERROR, "failed to open dataset %s", fsname);

	props = zfs_get_user_props(ds);
	VERIFY(props != NULL);

	if (nvlist_lookup_nvlist(props, "rfd77:ebox", &prop)) {
		errx(EXIT_ERROR, "no rfd77:ebox property "
		    "could be read on dataset %s", fsname);
	}

	VERIFY0(nvlist_lookup_string(prop, "value", &b64));

	/* We use this string for the recovery flavour text. */
	desclen = strlen(fsname) + 128;
	description = calloc(1, desclen);
	snprintf(description, desclen, "ZFS filesystem %s", fsname);

	buf = sshbuf_new();
	if (buf == NULL)
		err(EXIT_ERROR, "failed to allocate buffer");
	if ((rc = sshbuf_b64tod(buf, b64))) {
		error = ssherrf("sshbuf_b64tod", rc);
		errfx(EXIT_ERROR, error, "failed to parse rfd77:ebox property"
		    " on %s as base64", fsname);
	}
	if ((error = sshbuf_get_ebox(buf, &ebox))) {
		errfx(EXIT_ERROR, error, "failed to parse rfd77:ebox property"
		    " on %s as a valid ebox", fsname);
	}

	(void) mlockall(MCL_CURRENT | MCL_FUTURE);
	if ((error = unlock_or_recover(ebox, description, &recovered)))
		errfx(EXIT_ERROR, error, "failed to unlock ebox");

	key = ebox_key(ebox, &keylen);
#if defined(MADV_DONTDUMP)
	(void) madvise((void *)key, keylen, MADV_DONTDUMP);
#endif

	error = ebox_create(zfsebtpl, key, keylen, NULL, 0, &nebox);
	if (error)
		errfx(EXIT_ERROR, error, "ebox_create failed");
	sshbuf_reset(buf);
	error = sshbuf_put_ebox(buf, nebox);
	if (error)
		errfx(EXIT_ERROR, error, "sshbuf_put_ebox failed");

	b64 = sshbuf_dtob64(buf);

	rc = zfs_prop_set(ds, "rfd77:ebox", b64);
	if (rc != 0) {
		errno = rc;
		err(EXIT_ERROR, "failed to set ZFS property rfd77:ebox "
		    "on dataset %s", fsname);
	}

	free(b64);

	sshbuf_free(buf);
	ebox_free(ebox);
	ebox_free(nebox);
	zfs_close(ds);
}

static void
cmd_genopt(const char *cmd, const char *subcmd, const char *opt,
    const char *argv[], int argc)
{
	uint8_t *key;
	size_t keylen;
	uint i;
	int rc;
	const char **newargv;
	size_t newargc, maxargc;
	pid_t kid, rkid;
	int inpipe[2];
	ssize_t done;
	struct ebox *ebox;
	errf_t *error;
	struct sshbuf *buf;
	char *b64;

	if (zfsebtpl == NULL) {
		warnx("-t <tplname|path> option is required");
		usage();
	}

	key = calloc(1, 32);
	keylen = 32;

	(void) mlockall(MCL_CURRENT | MCL_FUTURE);
#if defined(MADV_DONTDUMP)
	(void) madvise(key, keylen, MADV_DONTDUMP);
#endif
	arc4random_buf(key, keylen);

	maxargc = argc + 10;
	newargv = calloc(maxargc, sizeof (char *));
	newargc = 0;

	newargv[newargc++] = cmd;
	newargv[newargc++] = subcmd;

	newargv[newargc++] = opt;
	newargv[newargc++] = "encryption=on";
	newargv[newargc++] = opt;
	newargv[newargc++] = "keyformat=raw";

	error = ebox_create(zfsebtpl, key, keylen, NULL, 0, &ebox);
	if (error)
		errfx(EXIT_ERROR, error, "ebox_create failed");
	buf = sshbuf_new();
	if (buf == NULL)
		errx(EXIT_ERROR, "failed to allocate memory");
	error = sshbuf_put_ebox(buf, ebox);
	if (error)
		errfx(EXIT_ERROR, error, "sshbuf_put_ebox failed");

	b64 = sshbuf_dtob64(buf);
	sshbuf_reset(buf);
	if ((rc = sshbuf_putf(buf, "rfd77:ebox=%s", b64)) ||
	    (rc = sshbuf_put_u8(buf, 0))) {
		error = ssherrf("sshbuf_put_*", rc);
		errfx(EXIT_ERROR, error, "failed writing to buffer");
	}

	newargv[newargc++] = opt;
	newargv[newargc++] = (const char *)sshbuf_ptr(buf);

	for (i = 0; i < argc; ++i)
		newargv[newargc++] = argv[i];

	newargv[newargc++] = 0;

	VERIFY0(pipe(inpipe));

	kid = fork();
	if (kid == -1) {
		perror("fork");
		exit(1);
	} else if (kid == 0) {
		VERIFY0(close(inpipe[1]));
		VERIFY0(dup2(inpipe[0], STDIN_FILENO));
		VERIFY0(close(inpipe[0]));
		VERIFY0(execvp(cmd, (char * const *)newargv));
	} else {
		VERIFY0(close(inpipe[0]));
		done = write(inpipe[1], key, keylen);
		VERIFY3S(done, ==, keylen);
		VERIFY0(close(inpipe[1]));

		rkid = waitpid(kid, &rc, 0);
		VERIFY3S(rkid, ==, kid);
		if (!WIFEXITED(rc)) {
			fprintf(stderr, "error: child did not exit\n");
			exit(1);
		}
		exit(WEXITSTATUS(rc));
	}
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: pivy-zfs [-d] [-t tplname] operation\n"
	    "Options:\n"
	    "  -d                      Debug mode\n"
	    "  -t tplname              Specify ebox template name\n"
	    "\n"
	    "Available operations:\n"
	    "  unlock <zfs>            Unlock an encrypted ZFS filesystem\n"
	    "  zfs-create -- <args>    Run 'zfs create' with arguments and\n"
	    "                          input transformed to provide keys for\n"
	    "                          encryption.\n"
	    "  zpool-create -- <args>  Like zfs-create but used to create a\n"
	    "                          new pool\n"
	    "  rekey <zfs>             Change key configuration for an already\n"
	    "                          created ZFS filesystem\n");
	fprintf(stderr, "\nTemplates are stored in " TPL_DEFAULT_PATH
	    " (manage them using the `pivy-box' tool)\n", "$HOME", "*");
	exit(EXIT_USAGE);
}

int
main(int argc, char *argv[])
{
	extern char *optarg;
	extern int optind;
	int c;
	const char *optstring = "t:d";
	const char *tpl = NULL;

	qa_term_setup();

	bunyan_init();
	bunyan_set_name("piv-zfs");

	while ((c = getopt(argc, argv, optstring)) != -1) {
		switch (c) {
		case 'd':
			bunyan_set_level(TRACE);
			break;
		case 't':
			tpl = optarg;
			break;
		}
	}

	if (optind >= argc) {
		warnx("operation required");
		usage();
	}

	if (tpl != NULL)
		zfsebtpl = read_tpl_file(tpl);

	const char *op = argv[optind++];

	zfshdl = libzfs_init();

	if (strcmp(op, "unlock") == 0) {
		const char *fsname;

		if (optind >= argc) {
			warnx("target zfs required");
			usage();
		}
		fsname = argv[optind++];

		if (optind < argc) {
			warnx("too many arguments");
			usage();
		}

		cmd_unlock(fsname);

	} else if (strcmp(op, "rekey") == 0) {
		const char *fsname;

		if (optind >= argc) {
			warnx("target zfs required");
			usage();
		}
		fsname = argv[optind++];

		if (optind < argc) {
			warnx("too many arguments");
			usage();
		}

		cmd_rekey(fsname);

	} else if (strcmp(op, "zfs-create") == 0) {
		if (optind >= argc) {
			warnx("zfs create args required");
			usage();
		}
		cmd_genopt("zfs", "create", "-o",
		    (const char **)&argv[optind], argc - optind);

	} else if (strcmp(op, "zpool-create") == 0) {
		if (optind >= argc) {
			warnx("zpool create args required");
			usage();
		}
		cmd_genopt("zpool", "create", "-O",
		    (const char **)&argv[optind], argc - optind);

	} else {
		warnx("unknown operation '%s'", op);
		usage();
	}

	libzfs_fini(zfshdl);

	return (0);
}
