/* 
   Unix SMB/CIFS implementation.
   status reporting
   Copyright (C) Andrew Tridgell 1994-1998

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

   Revision History:

   12 aug 96: Erik.Devriendt@te6.siemens.be
   added support for shared memory implementation of share mode locking

   21-Jul-1998: rsharpe@ns.aus.com (Richard Sharpe)
   Added -L (locks only) -S (shares only) flags and code

*/

/*
 * This program reports current SMB connections
 */

#include "includes.h"
#include "lib/util/server_id.h"
#include "smbd/globals.h"
#include "system/filesys.h"
#include "lib/cmdline/cmdline.h"
#include "dbwrap/dbwrap.h"
#include "dbwrap/dbwrap_open.h"
#include "../libcli/security/security.h"
#include "session.h"
#include "locking/share_mode_lock.h"
#include "locking/proto.h"
#include "messages.h"
#include "librpc/gen_ndr/open_files.h"
#include "smbd/smbd.h"
#include "librpc/gen_ndr/notify.h"
#include "conn_tdb.h"
#include "serverid.h"
#include "status_profile.h"
#include "smbd/notifyd/notifyd_db.h"
#include "cmdline_contexts.h"
#include "locking/leases_db.h"
#include "lib/util/string_wrappers.h"

#ifdef HAVE_JANSSON
#include <jansson.h>
#include "audit_logging.h" /* various JSON helpers */
#include "auth/common_auth.h"
#endif /* [HAVE_JANSSON] */

#define SMB_MAXPIDS		2048
static uid_t 		Ucrit_uid = 0;               /* added by OH */
static struct server_id	Ucrit_pid[SMB_MAXPIDS];  /* Ugly !!! */   /* added by OH */
static int		Ucrit_MaxPid=0;                    /* added by OH */
static unsigned int	Ucrit_IsActive = 0;                /* added by OH */

static bool verbose, brief;
static bool shares_only;            /* Added by RJS */
static bool locks_only;            /* Added by RJS */
static bool processes_only;
static bool show_brl;
static bool numeric_only;
static bool do_checks = true;
static bool json_output = false;
static bool csv_output = false;
static bool resolve_uids = false;

const char *username = NULL;
const char *session_id = NULL;

static const char *session_dialect_str(uint16_t dialect);

#ifdef HAVE_JANSSON
struct txt2mask {
	char	*field;
	int	mask;
};

struct txt2mask accessmask[] = {
	{"READ_DATA", FILE_READ_DATA},
	{"WRITE_DATA", FILE_WRITE_DATA},
	{"APPEND_DATA", FILE_APPEND_DATA},
	{"READ_EA", FILE_READ_EA},
	{"WRITE_EA", FILE_WRITE_EA},
	{"EXECUTE", FILE_EXECUTE},
	{"READ_ATTRIBUTES", FILE_READ_ATTRIBUTES},
	{"WRITE_ATTRIBUTES", FILE_WRITE_ATTRIBUTES},
	{"DELETE_CHILD", FILE_DELETE_CHILD},
	{"DELETE", SEC_STD_DELETE},
	{"READ_CONTROL", SEC_STD_READ_CONTROL},
	{"WRITE_DAC", SEC_STD_WRITE_DAC},
	{"SYNCHRONIZE", SEC_STD_SYNCHRONIZE},
	{"ACCESS_SYSTEM_SECURITY", SEC_FLAG_SYSTEM_SECURITY},
	{"MAXIMUM_ALLOWED", SEC_FLAG_MAXIMUM_ALLOWED},
	{"GENERIC_ALL", SEC_GENERIC_ALL},
	{"GENERIC_EXECUTE", SEC_GENERIC_EXECUTE},
	{"GENERIC_WRITE", SEC_GENERIC_WRITE},
	{"GENERIC_READ", SEC_GENERIC_READ},
	{0, 0}
};

struct txt2mask oplockmask[] = {
	{"EXCLUSIVE", EXCLUSIVE_OPLOCK},
	{"BATCH", BATCH_OPLOCK},
	{"LEVEL_II", LEVEL_II_OPLOCK},
	{"LEASE", LEASE_OPLOCK},
	{0, 0}
};

struct txt2mask leasemask[] = {
	{"READ", SMB2_LEASE_READ},
	{"WRITE", SMB2_LEASE_WRITE},
	{"HANDLE", SMB2_LEASE_HANDLE},
	{0, 0}
};

/*
 * Convert a mask of some sort (access, oplock, leases),
 * to key/value pairs in a JSON object.
 */
static int map_json_mask(struct json_object *jsobj,
                         int tomap,
                         const struct txt2mask *table)
{
        const struct txt2mask *a;
        int ret = 0;
        for (a = table; a->field !=0; a++) {
                if (json_add_bool(jsobj,
                                  a->field,
                                  (tomap & a->mask)
                                  ?true:false) < 0){
                        return -1;
                }
        }

        return 0;
}
#endif /* [HAVE_JANSSON] */

/* added by OH */
static void Ucrit_addUid(uid_t uid)
{
	Ucrit_uid = uid;
	Ucrit_IsActive = 1;
}

static unsigned int Ucrit_checkUid(uid_t uid)
{
	if ( !Ucrit_IsActive ) 
		return 1;

	if ( uid == Ucrit_uid ) 
		return 1;

	return 0;
}

static unsigned int Ucrit_checkPid(struct server_id pid)
{
	int i;

	if ( !Ucrit_IsActive ) 
		return 1;

	for (i=0;i<Ucrit_MaxPid;i++) {
		if (server_id_equal(&pid, &Ucrit_pid[i])) {
			return 1;
		}
	}

	return 0;
}

static bool Ucrit_addPid( struct server_id pid )
{
	if ( !Ucrit_IsActive )
		return True;

	if ( Ucrit_MaxPid >= SMB_MAXPIDS ) {
		d_printf("ERROR: More than %d pids for user %s!\n",
			 SMB_MAXPIDS, uidtoname(Ucrit_uid));

		return False;
	}

	Ucrit_pid[Ucrit_MaxPid++] = pid;

	return True;
}

#ifdef HAVE_JANSSON
struct print_share_mode_state {
	char *session_id;
	struct json_object jsobj;
};

static int print_share_mode_json(struct file_id fid,
				 const struct share_mode_data *d,
				 const struct share_mode_entry *e,
				 void *private_data)
{
	TALLOC_CTX *tmp_ctx = talloc_new(NULL);
	if (tmp_ctx == NULL) {
		return -1;
	}
	struct print_share_mode_state *state = NULL;
	struct json_object jsobj;
	struct json_object jsobjint;
	state = talloc_get_type_abort(private_data, struct print_share_mode_state);
	jsobj = state->jsobj;
	jsobjint = json_new_object();
	static int count;
	const char *denymode = NULL;
	char *access_mask = NULL;
	const char *lstate_hex = NULL;

	if (do_checks && !is_valid_share_mode_entry(e)) {
		TALLOC_FREE(tmp_ctx);
		json_free(&jsobjint);
		return 0;
	}

	count++;

	if (do_checks && !serverid_exists(&e->pid)) {
		/* the process for this entry does not exist any more */
		TALLOC_FREE(tmp_ctx);
		json_free(&jsobjint);
		return 0;
	}

	if (state->session_id) {
		struct server_id_buf tmp;
		char *s_id = server_id_str_buf(e->pid, &tmp);
		if (strcmp(s_id, state->session_id) != 0) {
			TALLOC_FREE(tmp_ctx);
			json_free(&jsobjint);
			return 0;
		}
	}
	if (Ucrit_checkPid(e->pid)) {
		struct server_id_buf tmp;
		uint denymode_int = 0;
		denymode_int = map_share_mode_to_deny_mode(e->share_access,
							   e->private_options);

		if (json_is_invalid(&jsobjint)) {
			return -1;
		}
		if (json_add_string(&jsobjint, "session_id",
				    server_id_str_buf(e->pid, &tmp)) < 0) {
			goto failure;
		}
		if (resolve_uids && (json_add_string(&jsobjint, "username",
					    uidtoname(e->uid)) < 0)) {
			goto failure;
		}
		if (json_add_int(&jsobjint, "uid", (unsigned int)e->uid) < 0) {
			goto failure;
		}
		switch (denymode_int) {
			case DENY_NONE: denymode = "DENY_NONE"; break;
			case DENY_ALL:  denymode = "DENY_ALL"; break;
			case DENY_DOS:  denymode = "DENY_DOS"; break;
			case DENY_READ: denymode = "DENY_READ"; break;
			case DENY_WRITE:denymode = "DENY_WRITE"; break;
			case DENY_FCB:  denymode = "DENY_FCB"; break;
			default: {
				denymode = talloc_asprintf(tmp_ctx,
							   "UNKNOWN(0x%08x)",
							   denymode_int);
				break;
			}
		}
	        struct json_object amask;
	        amask = json_new_object();
		if (json_is_invalid(&amask)) {
			goto failure;
		}
		access_mask = talloc_asprintf(tmp_ctx, "0x%08x",
					      (unsigned int)e->access_mask);
		if (json_add_string(&amask, "raw", access_mask) < 0) {
			goto failure;
		}
		TALLOC_FREE(access_mask);
		if (verbose) {
			if (map_json_mask(&amask, e->access_mask, accessmask) < 0) {
				json_free(&amask);
				goto failure;
			}
		}
		if (json_add_object(&jsobjint, "access_mask", &amask) < 0) {
			json_free(&amask);
			goto failure;
		}

		struct json_object oplock;
		oplock = json_new_object();
		if (json_is_invalid(&oplock)) {
			goto failure;
		}

		struct txt2mask *o;
		if (map_json_mask(&oplock, e->op_type, oplockmask) < 0) {
			json_free(&amask);
			goto failure;
		}
		if (json_add_object(&jsobjint, "oplock", &oplock) < 0) {
			json_free(&oplock);
			goto failure;
		}

		struct json_object lease;
		lease = json_new_object();
		if (json_is_invalid(&lease)) {
			goto failure;
		}
		if (e->op_type & LEASE_OPLOCK) {
			NTSTATUS status;
			uint32_t lstate;

			status = leases_db_get(
				&e->client_guid,
				&e->lease_key,
				&d->id,
				&lstate, /* current_state */
				NULL, /* breaking */
				NULL, /* breaking_to_requested */
				NULL, /* breaking_to_required */
				NULL, /* lease_version */
				NULL); /* epoch */

			if (NT_STATUS_IS_OK(status)) {
				if (map_json_mask(&lease,
						  lstate,
						  leasemask) < 0) {
					json_free(&lease);
					goto failure;
				}
			}
			if (lstate > SMB2_LEASE_WRITE) {
				const char *lstate_hex;
				lstate_hex = talloc_asprintf(tmp_ctx,
							     "0x%04x",
							     lstate);
				if (json_add_bool(&lease,
						  "UNKNOWN",
						  lstate_hex) < 0) {
					json_free(&lease);
					goto failure;
				}
			}
			if (json_add_object(&jsobjint, "lease", &lease) < 0) {
				json_free(&lease);
				goto failure;
			}
		}
		else {
			if (map_json_mask(&lease, 0, leasemask) < 0) {
				json_free(&lease);
				goto failure;
			}
			if (json_add_object(&jsobjint, "lease", &lease) < 0) {
				json_free(&lease);
				goto failure;
			}
		}
		if (json_add_string(&jsobjint, "service_path", d->servicepath) < 0) {
			goto failure;
		}
		char *filename = NULL;
		filename = talloc_asprintf(tmp_ctx, "%s%s", d->base_name,
			(d->stream_name != NULL) ? d->stream_name : "");
		if (json_add_string(&jsobjint, "filename", filename) < 0) {
			TALLOC_FREE(filename);
			goto failure;
		}
		if (json_add_object(&jsobj, NULL, &jsobjint) < 0) {
			goto failure;
		}
	}
	TALLOC_FREE(tmp_ctx);
	return 0;

failure:
	TALLOC_FREE(tmp_ctx);
	json_free(&jsobjint);
	return -1;
}

static void print_brl_json(struct file_id id,
			struct server_id pid,
			enum brl_type lock_type,
			enum brl_flavour lock_flav,
			br_off start,
			br_off size,
			void *private_data)
{
	struct json_object jsobj;
	struct json_object jsobjint;
	jsobj = *(struct json_object *)private_data;
	jsobjint = json_new_object();
	static int count;
	unsigned int i;
	static const struct {
		enum brl_type lock_type;
		const char *desc;
	} lock_types[] = {
		{ READ_LOCK, "R" },
		{ WRITE_LOCK, "W" },
		{ UNLOCK_LOCK, "U" }
	};
	const char *desc="X";
	const char *sharepath = "";
	char *fname = NULL;
	struct share_mode_lock *share_mode;
	struct server_id_buf tmp;
	struct file_id_buf ftmp;

	count++;

	share_mode = fetch_share_mode_unlocked(NULL, id);
	if (share_mode) {
		fname = share_mode_filename(NULL, share_mode);
	} else {
		fname = talloc_strdup(NULL, "");
		if (fname == NULL) {
			return;
		}
	}

	for (i=0;i<ARRAY_SIZE(lock_types);i++) {
		if (lock_type == lock_types[i].lock_type) {
			desc = lock_types[i].desc;
		}
	}
	if (json_is_invalid(&jsobjint)) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "pid",
			    server_id_str_buf(pid, &tmp)) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "dev_inode",
			    file_id_str_buf(id, &ftmp)) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "read_write", desc) < 0) {
		goto failure;
	}
	if (json_add_int(&jsobjint, "start", (intmax_t)start) < 0) {
		goto failure;
	}
	if (json_add_int(&jsobjint, "size", (intmax_t)size) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "share_path", sharepath) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "file_name", fname) < 0) {
		goto failure;
	}
	if (json_add_object(&jsobj, NULL, &jsobjint) < 0) {
		goto failure;
	}

	TALLOC_FREE(fname);
	TALLOC_FREE(share_mode);

failure:
	TALLOC_FREE(fname);
	TALLOC_FREE(share_mode);
	json_free(&jsobjint);
}

static int traverse_connections_json(const struct connections_key *key,
				const struct connections_data *crec,
				void *private_data)
{
	TALLOC_CTX *tmp_ctx = talloc_new(NULL);
	if (tmp_ctx == NULL) {
		return -1;
	}
	struct json_object jsobj;
	struct json_object jsobjint;
	jsobj = *(struct json_object *)private_data;
	jsobjint = json_new_object();
	struct server_id_buf tmp;
	char *timestr = NULL;
	int result = 0;
	const char *encryption = "-";
	const char *signing = "-";
	if (json_is_invalid(&jsobjint)) {
		TALLOC_FREE(tmp_ctx);
		return -1;
	}

	if (crec->cnum == TID_FIELD_INVALID)
		TALLOC_FREE(tmp_ctx);
		json_free(&jsobjint);
		return 0;

	if (do_checks &&
	    (!process_exists(crec->pid) || !Ucrit_checkUid(crec->uid))) {
		TALLOC_FREE(tmp_ctx);
		json_free(&jsobjint);
		return 0;
	}

	timestr = timestring(tmp_ctx, crec->start);
	if (timestr == NULL) {
		goto failure;
	}

	if (smbXsrv_is_encrypted(crec->encryption_flags)) {
		switch (crec->cipher) {
		case SMB_ENCRYPTION_GSSAPI:
			encryption = "GSSAPI";
			break;
		case SMB2_ENCRYPTION_AES128_CCM:
			encryption = "AES-128-CCM";
			break;
		case SMB2_ENCRYPTION_AES128_GCM:
			encryption = "AES-128-GCM";
			break;
		default:
			encryption = talloc_asprintf(tmp_ctx,
						     "UNKNOWN(0x%08x)",
						     crec->cipher);
			result = -1;
			break;
		}
	}

	if (smbXsrv_is_signed(crec->signing_flags)) {
		switch (crec->dialect) {
		case SMB3_DIALECT_REVISION_311:
		case SMB3_DIALECT_REVISION_302:
			signing = "AES-128-CMAC";
			break;
		case SMB2_DIALECT_REVISION_2FF:
		case SMB2_DIALECT_REVISION_224:
		case SMB2_DIALECT_REVISION_222:
		case SMB2_DIALECT_REVISION_210:
		case SMB2_DIALECT_REVISION_202:
			signing = "HMAC-SHA256";
			break;
		case SMB2_DIALECT_REVISION_000:
			signing = "HMAC-MD5";
			break;
		default:
			signing = talloc_asprintf(tmp_ctx,
						  "UNKNOWN_DIALECT(0x%08x)",
						  crec->dialect);
			result = -1;
			break;
		}
	}

	if (json_add_string(&jsobjint, "service", crec->servicename) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "pid",
			    server_id_str_buf(crec->pid, &tmp)) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "machine", crec->machine) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "connected_at", timestr) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "encryption", encryption) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "signing", signing) < 0) {
		goto failure;
	}
	if (json_add_object(&jsobj, NULL, &jsobjint) < 0) {
		goto failure;
	}

	TALLOC_FREE(tmp_ctx);
	return result;

failure:
	TALLOC_FREE(tmp_ctx);
	json_free(&jsobjint);
	return -1;
}

static int traverse_sessionid_json(const char *key, struct sessionid *session,
				    void *private_data)
{
	TALLOC_CTX *tmp_ctx = talloc_new(NULL);
	if (tmp_ctx == NULL) {
		return -1;
	}
	struct json_object jsobj;
	struct json_object jsobjint;
	jsobj = *(struct json_object *)private_data;
	jsobjint = json_new_object();
	struct server_id_buf tmp;
	int result = 0;
	const char *encryption = "-";
	const char *signing = "-";

	if (do_checks &&
	    (!process_exists(session->pid) ||
	     !Ucrit_checkUid(session->uid))) {
		return 0;
	}

	Ucrit_addPid(session->pid);

	if (json_is_invalid(&jsobjint)) {
		return -1;
	}

	if (json_add_string(&jsobjint, "session_id",
			 server_id_str_buf(session->pid, &tmp)) < 0) {
		goto failure;
	}
	if (json_add_int(&jsobjint, "uid",
			 (unsigned int)session->uid) < 0) {
		goto failure;
	}
	if (json_add_int(&jsobjint, "gid",
			 (unsigned int)session->gid) < 0) {
		goto failure;
	}
	if (!numeric_only) {
		if (session->uid == -1 && session->gid == -1) {
			/*
			 * The session is not fully authenticated yet.
			 */
			if (json_add_string(&jsobjint, "username",
					    "(auth in progress)") < 0) {
				goto failure;
			}
			if (json_add_string(&jsobjint, "groupname",
					    "(auth in progress)") < 0) {
				goto failure;
			}
		} else {
			/*
			 * In theory it should not happen that one of
			 * session->uid and session->gid is valid (ie != -1)
			 * while the other is not (ie = -1), so we a check for
			 * that case that bails out would be reasonable.
			 */
			const char *uid_name = "-1";
			const char *gid_name = "-1";

			if (session->uid != -1) {
				uid_name = uidtoname(session->uid);
				if (uid_name == NULL) {
					return -1;
				}
			}
			if (session->gid != -1) {
				gid_name = gidtoname(session->gid);
				if (gid_name == NULL) {
					return -1;
				}
			}
			if (json_add_string(&jsobjint, "username",
					    uid_name) < 0) {
				goto failure;
			}
			if (json_add_string(&jsobjint, "groupname",
					    gid_name) < 0) {
				goto failure;
			}
		}
	}

	if (smbXsrv_is_encrypted(session->encryption_flags)) {
		switch (session->cipher) {
		case SMB2_ENCRYPTION_AES128_CCM:
			encryption = "AES-128-CCM";
			break;
		case SMB2_ENCRYPTION_AES128_GCM:
			encryption = "AES-128-GCM";
			break;
		default:
			encryption = talloc_asprintf(tmp_ctx,
						     "UNKNOWN(0x%08x)",
						     session->cipher);
			result = -1;
			break;
		}
	} else if (smbXsrv_is_partially_encrypted(session->encryption_flags)) {
		switch (session->cipher) {
		case SMB_ENCRYPTION_GSSAPI:
			encryption = "partial(GSSAPI)";
			break;
		case SMB2_ENCRYPTION_AES128_CCM:
			encryption = "partial(AES-128-CCM)";
			break;
		case SMB2_ENCRYPTION_AES128_GCM:
			encryption = "partial(AES-128-GCM)";
			break;
		default:
			encryption = talloc_asprintf(tmp_ctx,
						     "UNKNOWN(0x%08x)",
						     session->cipher);
			result = -1;
			break;
		}
	}

	if (smbXsrv_is_signed(session->signing_flags)) {
		switch (session->connection_dialect) {
		case SMB3_DIALECT_REVISION_311:
		case SMB3_DIALECT_REVISION_302:
			signing = "AES-128-CMAC";
			break;
		case SMB2_DIALECT_REVISION_2FF:
		case SMB2_DIALECT_REVISION_224:
		case SMB2_DIALECT_REVISION_222:
		case SMB2_DIALECT_REVISION_210:
		case SMB2_DIALECT_REVISION_202:
			signing = "HMAC-SHA256";
			break;
		case SMB2_DIALECT_REVISION_000:
			signing = "HMAC-MD5";
			break;
		default:
			signing = talloc_asprintf(tmp_ctx,
						  "UNKNOWN_DIALECT(0x%08x)",
						  session->connection_dialect);
			result = -1;
			break;
		}
	} else if (smbXsrv_is_partially_signed(session->signing_flags)) {
		switch (session->connection_dialect) {
		case SMB3_DIALECT_REVISION_311:
		case SMB3_DIALECT_REVISION_302:
			signing = "AES-128-CMAC";
			break;
		case SMB2_DIALECT_REVISION_2FF:
		case SMB2_DIALECT_REVISION_224:
		case SMB2_DIALECT_REVISION_222:
		case SMB2_DIALECT_REVISION_210:
		case SMB2_DIALECT_REVISION_202:
			signing = "HMAC-SHA256";
			break;
		case SMB2_DIALECT_REVISION_000:
			signing = "HMAC-MD5";
			break;
		default:
			signing = talloc_asprintf(tmp_ctx,
						  "UNKNOWN_DIALECT(0x%08x)",
						  session->connection_dialect);
			result = -1;
			break;
		}
	}

	if (json_add_string(&jsobjint, "remote_machine",
			    session->remote_machine) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "hostname", session->hostname) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "session_dialect",
		    session_dialect_str(session->connection_dialect)) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "encryption", encryption) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "signing", signing) < 0) {
		goto failure;
	}
	if (json_add_object(&jsobj, NULL, &jsobjint) < 0) {
		goto failure;
	}

	TALLOC_FREE(tmp_ctx);
	return result;

failure:
	TALLOC_FREE(tmp_ctx);
	json_free(&jsobjint);
	return -1;
}

static bool print_notify_rec_json(const char *path, struct server_id server,
			     const struct notify_instance *instance,
			     void *private_data)
{
	struct server_id_buf idbuf;
	struct json_object jsobj;
	struct json_object jsobjint;
	jsobj = *(struct json_object *)private_data;
	jsobjint = json_new_object();

	if (json_is_invalid(&jsobjint)) {
		return false;
	}
	if (json_add_string(&jsobjint, "pid", server_id_str_buf(server, &idbuf)) < 0) {
		goto failure;
	}
	if (json_add_string(&jsobjint, "path", path) < 0) {
		goto failure;
	}
	if (json_add_int(&jsobjint, "filter", (unsigned)instance->filter) < 0) {
		goto failure;
	}
	if (json_add_int(&jsobjint, "subdir_filter", (unsigned)instance->subdir_filter) < 0) {
		goto failure;
	}
	if (json_add_object(&jsobj, NULL, &jsobjint) < 0) {
		goto failure;
	}

	return true;

failure:
	json_free(&jsobjint);
	return false;
}
#endif /* [HAVE_JANSSON] */

static int print_share_mode(struct file_id fid,
			    const struct share_mode_data *d,
			    const struct share_mode_entry *e,
			    void *private_data)
{
	static int count;

	if (do_checks && !is_valid_share_mode_entry(e)) {
		return 0;
	}

	if (count==0) {
		d_printf("Locked files:\n");
		d_printf("Pid          User(ID)   DenyMode   Access      R/W        Oplock           SharePath   Name   Time\n");
		d_printf("--------------------------------------------------------------------------------------------------\n");
	}
	count++;

	if (do_checks && !serverid_exists(&e->pid)) {
		/* the process for this entry does not exist any more */
		return 0;
	}

	if (Ucrit_checkPid(e->pid)) {
		struct server_id_buf tmp;
		d_printf("%-11s  ", server_id_str_buf(e->pid, &tmp));
		if (resolve_uids) {
			d_printf("%-14s  ", uidtoname(e->uid));
		} else {
			d_printf("%-9u  ", (unsigned int)e->uid);
		}
		switch (map_share_mode_to_deny_mode(e->share_access,
						    e->private_options)) {
			case DENY_NONE: d_printf("DENY_NONE  "); break;
			case DENY_ALL:  d_printf("DENY_ALL   "); break;
			case DENY_DOS:  d_printf("DENY_DOS   "); break;
			case DENY_READ: d_printf("DENY_READ  "); break;
			case DENY_WRITE:d_printf("DENY_WRITE "); break;
			case DENY_FCB:  d_printf("DENY_FCB   "); break;
			default: {
				d_printf("unknown-please report ! "
					 "e->share_access = 0x%x, "
					 "e->private_options = 0x%x\n",
					 (unsigned int)e->share_access,
					 (unsigned int)e->private_options );
				break;
			}
		}
		d_printf("0x%-8x  ",(unsigned int)e->access_mask);
		if ((e->access_mask & (FILE_READ_DATA|FILE_WRITE_DATA))==
				(FILE_READ_DATA|FILE_WRITE_DATA)) {
			d_printf("RDWR       ");
		} else if (e->access_mask & FILE_WRITE_DATA) {
			d_printf("WRONLY     ");
		} else {
			d_printf("RDONLY     ");
		}

		if((e->op_type & (EXCLUSIVE_OPLOCK|BATCH_OPLOCK)) == 
					(EXCLUSIVE_OPLOCK|BATCH_OPLOCK)) {
			d_printf("EXCLUSIVE+BATCH ");
		} else if (e->op_type & EXCLUSIVE_OPLOCK) {
			d_printf("EXCLUSIVE       ");
		} else if (e->op_type & BATCH_OPLOCK) {
			d_printf("BATCH           ");
		} else if (e->op_type & LEVEL_II_OPLOCK) {
			d_printf("LEVEL_II        ");
		} else if (e->op_type == LEASE_OPLOCK) {
			NTSTATUS status;
			uint32_t lstate;

			status = leases_db_get(
				&e->client_guid,
				&e->lease_key,
				&d->id,
				&lstate, /* current_state */
				NULL, /* breaking */
				NULL, /* breaking_to_requested */
				NULL, /* breaking_to_required */
				NULL, /* lease_version */
				NULL); /* epoch */

			if (NT_STATUS_IS_OK(status)) {
				d_printf("LEASE(%s%s%s)%s%s%s      ",
					 (lstate & SMB2_LEASE_READ)?"R":"",
					 (lstate & SMB2_LEASE_WRITE)?"W":"",
					 (lstate & SMB2_LEASE_HANDLE)?"H":"",
					 (lstate & SMB2_LEASE_READ)?"":" ",
					 (lstate & SMB2_LEASE_WRITE)?"":" ",
					 (lstate & SMB2_LEASE_HANDLE)?"":" ");
			} else {
				d_printf("LEASE STATE UNKNOWN");
			}
		} else {
			d_printf("NONE            ");
		}

		d_printf(" %s   %s%s   %s",
			 d->servicepath, d->base_name,
			 (d->stream_name != NULL) ? d->stream_name : "",
			 time_to_asc((time_t)e->time.tv_sec));
	}

	return 0;
}

static void print_brl(struct file_id id,
			struct server_id pid, 
			enum brl_type lock_type,
			enum brl_flavour lock_flav,
			br_off start,
			br_off size,
			void *private_data)
{
	static int count;
	unsigned int i;
	static const struct {
		enum brl_type lock_type;
		const char *desc;
	} lock_types[] = {
		{ READ_LOCK, "R" },
		{ WRITE_LOCK, "W" },
		{ UNLOCK_LOCK, "U" }
	};
	const char *desc="X";
	const char *sharepath = "";
	char *fname = NULL;
	struct share_mode_lock *share_mode;
	struct server_id_buf tmp;
	struct file_id_buf ftmp;

	if (count==0) {
		d_printf("Byte range locks:\n");
		d_printf("Pid        dev:inode       R/W  start     size      SharePath               Name\n");
		d_printf("--------------------------------------------------------------------------------\n");
	}
	count++;

	share_mode = fetch_share_mode_unlocked(NULL, id);
	if (share_mode) {
		fname = share_mode_filename(NULL, share_mode);
	} else {
		fname = talloc_strdup(NULL, "");
		if (fname == NULL) {
			return;
		}
	}

	for (i=0;i<ARRAY_SIZE(lock_types);i++) {
		if (lock_type == lock_types[i].lock_type) {
			desc = lock_types[i].desc;
		}
	}

	d_printf("%-10s %-15s %-4s %-9jd %-9jd %-24s %-24s\n",
		 server_id_str_buf(pid, &tmp),
		 file_id_str_buf(id, &ftmp),
		 desc,
		 (intmax_t)start, (intmax_t)size,
		 sharepath, fname);

	TALLOC_FREE(fname);
	TALLOC_FREE(share_mode);
}

static const char *session_dialect_str(uint16_t dialect)
{
	static fstring unkown_dialect;

	switch(dialect){
	case SMB2_DIALECT_REVISION_000:
		return "NT1";
	case SMB2_DIALECT_REVISION_202:
		return "SMB2_02";
	case SMB2_DIALECT_REVISION_210:
		return "SMB2_10";
	case SMB2_DIALECT_REVISION_222:
		return "SMB2_22";
	case SMB2_DIALECT_REVISION_224:
		return "SMB2_24";
	case SMB3_DIALECT_REVISION_300:
		return "SMB3_00";
	case SMB3_DIALECT_REVISION_302:
		return "SMB3_02";
	case SMB3_DIALECT_REVISION_310:
		return "SMB3_10";
	case SMB3_DIALECT_REVISION_311:
		return "SMB3_11";
	}

	fstr_sprintf(unkown_dialect, "Unknown (0x%04x)", dialect);
	return unkown_dialect;
}

static int traverse_connections(const struct connections_key *key,
				const struct connections_data *crec,
				void *private_data)
{
	TALLOC_CTX *mem_ctx = (TALLOC_CTX *)private_data;
	struct server_id_buf tmp;
	char *timestr = NULL;
	int result = 0;
	const char *encryption = "-";
	const char *signing = "-";

	if (crec->cnum == TID_FIELD_INVALID)
		return 0;

	if (do_checks &&
	    (!process_exists(crec->pid) || !Ucrit_checkUid(crec->uid))) {
		return 0;
	}

	timestr = timestring(mem_ctx, crec->start);
	if (timestr == NULL) {
		return -1;
	}

	if (smbXsrv_is_encrypted(crec->encryption_flags)) {
		switch (crec->cipher) {
		case SMB_ENCRYPTION_GSSAPI:
			encryption = "GSSAPI";
			break;
		case SMB2_ENCRYPTION_AES128_CCM:
			encryption = "AES-128-CCM";
			break;
		case SMB2_ENCRYPTION_AES128_GCM:
			encryption = "AES-128-GCM";
			break;
		default:
			encryption = "???";
			result = -1;
			break;
		}
	}

	if (smbXsrv_is_signed(crec->signing_flags)) {
		switch (crec->signing) {
		case SMB2_SIGNING_MD5_SMB1:
			signing = "HMAC-MD5";
			break;
		case SMB2_SIGNING_HMAC_SHA256:
			signing = "HMAC-SHA256";
			break;
		case SMB2_SIGNING_AES128_CMAC:
			signing = "AES-128-CMAC";
			break;
		case SMB2_SIGNING_AES128_GMAC:
			signing = "AES-128-GMAC";
			break;
		default:
			signing = "???";
			result = -1;
			break;
		}
	}

	d_printf("%-12s %-7s %-13s %-32s %-12s %-12s\n",
		 crec->servicename, server_id_str_buf(crec->pid, &tmp),
		 crec->machine,
		 timestr,
		 encryption,
		 signing);

	TALLOC_FREE(timestr);

	return result;
}

static int traverse_sessionid(const char *key, struct sessionid *session,
			      void *private_data)
{
	TALLOC_CTX *mem_ctx = (TALLOC_CTX *)private_data;
	fstring uid_gid_str;
	struct server_id_buf tmp;
	char *machine_hostname = NULL;
	int result = 0;
	const char *encryption = "-";
	const char *signing = "-";

	if (do_checks &&
	    (!process_exists(session->pid) ||
	     !Ucrit_checkUid(session->uid))) {
		return 0;
	}

	Ucrit_addPid(session->pid);

	if (numeric_only) {
		fstr_sprintf(uid_gid_str, "%-12u %-12u",
			     (unsigned int)session->uid,
			     (unsigned int)session->gid);
	} else {
		if (session->uid == -1 && session->gid == -1) {
			/*
			 * The session is not fully authenticated yet.
			 */
			fstrcpy(uid_gid_str, "(auth in progress)");
		} else {
			/*
			 * In theory it should not happen that one of
			 * session->uid and session->gid is valid (ie != -1)
			 * while the other is not (ie = -1), so we a check for
			 * that case that bails out would be reasonable.
			 */
			const char *uid_name = "-1";
			const char *gid_name = "-1";

			if (session->uid != -1) {
				uid_name = uidtoname(session->uid);
				if (uid_name == NULL) {
					return -1;
				}
			}
			if (session->gid != -1) {
				gid_name = gidtoname(session->gid);
				if (gid_name == NULL) {
					return -1;
				}
			}
			fstr_sprintf(uid_gid_str, "%-12s %-12s",
				     uid_name, gid_name);
		}
	}

	machine_hostname = talloc_asprintf(mem_ctx, "%s (%s)",
					   session->remote_machine,
					   session->hostname);
	if (machine_hostname == NULL) {
		return -1;
	}

	if (smbXsrv_is_encrypted(session->encryption_flags)) {
		switch (session->cipher) {
		case SMB2_ENCRYPTION_AES128_CCM:
			encryption = "AES-128-CCM";
			break;
		case SMB2_ENCRYPTION_AES128_GCM:
			encryption = "AES-128-GCM";
			break;
		case SMB2_ENCRYPTION_AES256_CCM:
			encryption = "AES-256-CCM";
			break;
		case SMB2_ENCRYPTION_AES256_GCM:
			encryption = "AES-256-GCM";
			break;
		default:
			encryption = "???";
			result = -1;
			break;
		}
	} else if (smbXsrv_is_partially_encrypted(session->encryption_flags)) {
		switch (session->cipher) {
		case SMB_ENCRYPTION_GSSAPI:
			encryption = "partial(GSSAPI)";
			break;
		case SMB2_ENCRYPTION_AES128_CCM:
			encryption = "partial(AES-128-CCM)";
			break;
		case SMB2_ENCRYPTION_AES128_GCM:
			encryption = "partial(AES-128-GCM)";
			break;
		case SMB2_ENCRYPTION_AES256_CCM:
			encryption = "partial(AES-256-CCM)";
			break;
		case SMB2_ENCRYPTION_AES256_GCM:
			encryption = "partial(AES-256-GCM)";
			break;
		default:
			encryption = "???";
			result = -1;
			break;
		}
	}

	if (smbXsrv_is_signed(session->signing_flags)) {
		switch (session->signing) {
		case SMB2_SIGNING_MD5_SMB1:
			signing = "HMAC-MD5";
			break;
		case SMB2_SIGNING_HMAC_SHA256:
			signing = "HMAC-SHA256";
			break;
		case SMB2_SIGNING_AES128_CMAC:
			signing = "AES-128-CMAC";
			break;
		case SMB2_SIGNING_AES128_GMAC:
			signing = "AES-128-GMAC";
			break;
		default:
			signing = "???";
			result = -1;
			break;
		}
	} else if (smbXsrv_is_partially_signed(session->signing_flags)) {
		switch (session->signing) {
		case SMB2_SIGNING_MD5_SMB1:
			signing = "partial(HMAC-MD5)";
			break;
		case SMB2_SIGNING_HMAC_SHA256:
			signing = "partial(HMAC-SHA256)";
			break;
		case SMB2_SIGNING_AES128_CMAC:
			signing = "partial(AES-128-CMAC)";
			break;
		case SMB2_SIGNING_AES128_GMAC:
			signing = "partial(AES-128-GMAC)";
			break;
		default:
			signing = "???";
			result = -1;
			break;
		}
	}


	d_printf("%-7s %-25s %-41s %-17s %-20s %-21s\n",
		 server_id_str_buf(session->pid, &tmp),
		 uid_gid_str,
		 machine_hostname,
		 session_dialect_str(session->connection_dialect),
		 encryption,
		 signing);

	TALLOC_FREE(machine_hostname);

	return result;
}

static bool print_notify_rec(const char *path, struct server_id server,
			     const struct notify_instance *instance,
			     void *private_data)
{
	struct server_id_buf idbuf;

	d_printf("%s\\%s\\%x\\%x\n", path, server_id_str_buf(server, &idbuf),
		 (unsigned)instance->filter,
		 (unsigned)instance->subdir_filter);

	return true;
}

enum {
	OPT_RESOLVE_UIDS = 1000,
};

int main(int argc, const char *argv[])
{
	int c;
	uint64_t interval = 1;
	int profile_only = 0;
	bool show_processes, show_locks, show_shares;
	bool show_notify = false;
	poptContext pc = NULL;
	enum PROFILE_OUTPUT pout = PROF_TEXT;
	struct poptOption long_options[] = {
		POPT_AUTOHELP
		{
			.longName   = "processes",
			.shortName  = 'p',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'p',
			.descrip    = "Show processes only",
		},
		{
			.longName   = "verbose",
			.shortName  = 'v',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'v',
			.descrip    = "Be verbose",
		},
		{
			.longName   = "locks",
			.shortName  = 'L',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'L',
			.descrip    = "Show locks only",
		},
		{
			.longName   = "shares",
			.shortName  = 'S',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'S',
			.descrip    = "Show shares only",
		},
		{
			.longName   = "notify",
			.shortName  = 'N',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'N',
			.descrip    = "Show notifies",
		},
		{
			.longName   = "user",
			.shortName  = 'u',
			.argInfo    = POPT_ARG_STRING,
			.arg        = &username,
			.val        = 'u',
			.descrip    = "Switch to user",
		},
		{
			.longName   = "session-id",
			.shortName  = 's',
			.argInfo    = POPT_ARG_STRING,
			.arg        = &session_id,
			.val        = 's',
			.descrip    = "Restrict to session id",
		},
		{
			.longName   = "brief",
			.shortName  = 'b',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'b',
			.descrip    = "Be brief",
		},
		{
			.longName   = "profile",
			.shortName  =     'P',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'P',
			.descrip    = "Do profiling",
		},
		{
			.longName   = "profile-rates",
			.shortName  = 'R',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'R',
			.descrip    = "Show call rates",
		},
		{
			.longName   = "profile-timed-dump",
			.shortName  = 'D',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'D',
			.descrip    = "Show call rates",
		},
		{
			.longName   = "byterange",
			.shortName  = 'B',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'B',
			.descrip    = "Include byte range locks"
		},
		{
			.longName   = "numeric",
			.shortName  = 'n',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'n',
			.descrip    = "Numeric uid/gid"
		},
		{
			.longName   = "json",
			.shortName  = 'j',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'j',
			.descrip    = "JSON output"
		},
		{
			.longName   = "csv",
			.shortName  = 'c',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'c',
			.descrip    = "CSV output"
		},
		{
			.longName   = "sample-interval",
			.shortName  = 'i',
			.argInfo    = POPT_ARG_INT,
			.arg        = &interval,
			.val        = 'i',
			.descrip    = "CSV output"
		},
		{
			.longName   = "fast",
			.shortName  = 'f',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'f',
			.descrip    = "Skip checks if processes still exist"
		},
		{
			.longName   = "resolve-uids",
			.shortName  = 0,
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = OPT_RESOLVE_UIDS,
			.descrip    = "Try to resolve UIDs to usernames"
		},
		POPT_COMMON_SAMBA
		POPT_COMMON_VERSION
		POPT_TABLEEND
	};
	TALLOC_CTX *frame = talloc_stackframe();
	int ret = 0;
	struct messaging_context *msg_ctx = NULL;
	char *db_path;
	bool ok;

	smb_init_locale();

	ok = samba_cmdline_init(frame,
				SAMBA_CMDLINE_CONFIG_CLIENT,
				false /* require_smbconf */);
	if (!ok) {
		DBG_ERR("Failed to init cmdline parser!\n");
		TALLOC_FREE(frame);
		exit(1);
	}
	lp_set_cmdline("log level", "0");

	#ifdef HAVE_JANSSON
	struct json_object jsobj = json_new_object();
	#else /* [HAVE_JANSSON] */
	if (json_output) {
		DBG_ERR("JSON support not available\n");
		goto done;
	}
	#endif /* [HAVE_JANSSON] */
	if (json_output && csv_output) {
		DBG_ERR("simultaneous CSV and json output not permitted\n");
		goto done;
	}

	pc = samba_popt_get_context(getprogname(),
				    argc,
				    argv,
				    long_options,
				    POPT_CONTEXT_KEEP_FIRST);
	if (pc == NULL) {
		DBG_ERR("Failed to setup popt context!\n");
		TALLOC_FREE(frame);
		exit(1);
	}

	while ((c = poptGetNextOpt(pc)) != -1) {
		switch (c) {
		case 'p':
			processes_only = true;
			break;
		case 'v':
			verbose = true;
			break;
		case 'L':
			locks_only = true;
			break;
		case 'S':
			shares_only = true;
			break;
		case 'N':
			show_notify = true;
			break;
		case 'b':
			brief = true;
			break;
		case 'u':
			Ucrit_addUid(nametouid(poptGetOptArg(pc)));
			break;
		case 's':
			break;
		case 'D':
		case 'P':
		case 'R':
			profile_only = c;
			break;
		case 'B':
			show_brl = true;
			break;
		case 'n':
			numeric_only = true;
			break;
		case 'j':
			json_output = true;
			pout = PROF_JSON;
			break;
		case 'c':
			csv_output = true;
			pout = PROF_CSV;
			break;
		case 'i':
			break;
		case 'f':
			do_checks = false;
			break;
		case OPT_RESOLVE_UIDS:
			resolve_uids = true;
			break;
		case POPT_ERROR_BADOPT:
			fprintf(stderr, "\nInvalid option %s: %s\n\n",
				poptBadOption(pc, 0), poptStrerror(c));
			poptPrintUsage(pc, stderr, 0);
			exit(1);
		}
	}

	sec_init();

	if (getuid() != geteuid()) {
		d_printf("smbstatus should not be run setuid\n");
		ret = 1;
		goto done;
	}

	if (getuid() != 0) {
		d_printf("smbstatus only works as root!\n");
		ret = 1;
		goto done;
	}

	/* setup the flags based on the possible combincations */

	show_processes = !(shares_only || locks_only || profile_only) || processes_only;
	show_locks     = !(shares_only || processes_only || profile_only) || locks_only;
	show_shares    = !(processes_only || locks_only || profile_only) || shares_only;

	if ( username )
		Ucrit_addUid( nametouid(username) );

	if (verbose && !json_output) {
		d_printf("using configfile = %s\n", get_dyn_CONFIGFILE());
	}

	msg_ctx = cmdline_messaging_context(get_dyn_CONFIGFILE());
	if (msg_ctx == NULL) {
		if (json_output) {
			fprintf(stderr, "Could not initialize messaging, not root?\n");
		}
		else {
			fprintf(stderr, "Could not initialize messaging, not root?\n");
		}
		ret = -1;
		goto done;
	}

	switch (profile_only) {
		case 'P':
			/* Dump profile data */
			ok = status_profile_dump(verbose, pout);
			ret = ok ? 0 : 1;
			goto done;
		case 'R':
			/* Continuously display rate-converted data */
			ok = status_profile_rates(verbose);
			ret = ok ? 0 : 1;
			goto done;
		case 'D':
			/* Continuously display profile dump (Big D)*/
			fprintf(stderr, "Sampling interval: %lu\n", interval);
			ok = status_profile_timed_dump(verbose, pout, interval);
			ret = ok ? 0 : 1;
			goto done;
		default:
			break;
	}

	if ( show_processes ) {
		if (!json_output) {
			d_printf("\nSamba version %s\n",samba_version_string());
			d_printf("%-7s %-12s %-12s %-41s %-17s %-20s %-21s\n", "PID", "Username", "Group", "Machine", "Protocol Version", "Encryption", "Signing");
			d_printf("----------------------------------------------------------------------------------------------------------------------------------------\n");
			sessionid_traverse_read(traverse_sessionid, frame);
		}
		#ifdef HAVE_JANSSON
		else {
			struct json_object sessions = json_new_array();
			if (json_is_invalid(&sessions)) {
				fprintf(stderr, "Failed to create JSON object [sessions]\n");
				ret = -1;
				goto done;
			}

			sessionid_traverse_read(traverse_sessionid_json, &sessions);
			if (json_add_object(&jsobj, "sessions", &sessions) < 0) {
				fprintf(stderr, "Failed to add JSON object [sessions]\n");
				json_free(&sessions);
				ret = -1;
				goto done;
			}
		}
		#endif /* [HAVE_JANSSON] */

		if (processes_only) {
			goto done;
		}
	}

	if ( show_shares ) {
		if (brief) {
			goto done;
		}

		if (!json_output) {
			d_printf("\n%-12s %-7s %-13s %-32s %-12s %-12s\n", "Service", "pid", "Machine", "Connected at", "Encryption", "Signing");
			d_printf("---------------------------------------------------------------------------------------------\n");
			connections_forall_read(traverse_connections, frame);
		}
		#ifdef HAVE_JANSSON
		else {
			struct json_object shares_array = json_new_array();
			if (json_is_invalid(&shares_array)) {
				fprintf(stderr, "Failed to create JSON array [shares_array]\n");
				ret = 1;
				goto done;
			}
			connections_forall_read(traverse_connections_json, &shares_array);
			if (json_add_object(&jsobj, "shares", &shares_array) < 0) {
				fprintf(stderr, "Failed to add JSON array [shares_array]\n");
				json_free(&shares_array);
				ret = -1;
				goto done;
			}

			if (!json_output) {
				d_printf("\n");
			}
		}
		#endif /* [HAVE_JANSSON] */

		if ( shares_only ) {
			goto done;
		}
	}

	if ( show_locks ) {
		int result;
		struct db_context *db;

		db_path = lock_path(talloc_tos(), "locking.tdb");
		if (db_path == NULL) {
			if (json_output) {
				fprintf(stderr, "Out of memory - exiting\n");
			}
			else {
				d_printf("Out of memory - exiting\n");
			}
			ret = -1;
			goto done;
		}

		db = db_open(NULL, db_path, 0,
			     TDB_CLEAR_IF_FIRST|TDB_INCOMPATIBLE_HASH, O_RDONLY, 0,
			     DBWRAP_LOCK_ORDER_1, DBWRAP_FLAG_NONE);

		if (!db) {
			if (json_output) {
				fprintf(stderr, "%s not initialised\n", db_path);
				fprintf(stderr, "This is normal if an SMB client has never "
					 "connected to your server.\n");
			}
			else {
				d_printf("%s not initialised\n", db_path);
				d_printf("This is normal if an SMB client has never "
					 "connected to your server.\n");
			}
			TALLOC_FREE(db_path);
			exit(0);
		} else {
			TALLOC_FREE(db);
			TALLOC_FREE(db_path);
		}

		if (!locking_init_readonly()) {
			d_printf("Can't initialise locking module - exiting\n");
			ret = 1;
			goto done;
		}
		if (!json_output) {
			result = share_entry_forall(print_share_mode, NULL);
		}
		#if HAVE_JANSSON
		else {
			struct json_object locks_array = json_new_array();
			struct print_share_mode_state *state = NULL;
			if (json_is_invalid(&locks_array)) {
				locking_end();
				fprintf(stderr, "Failed to create JSON array [locks_array]\n");
				ret = 1;
				goto done;
			}
			state = talloc_zero(NULL, struct print_share_mode_state);
			state->jsobj = locks_array;
			state->session_id = session_id;
			result = share_entry_forall(print_share_mode_json, state);
			if (json_add_object(&jsobj, "locked_files", &locks_array) < 0) {
				locking_end();
				fprintf(stderr, "Failed to add JSON array [locks_array]\n");
				ret = 1;
				goto done;
			}
			TALLOC_FREE(state);
		}
		#endif /* [HAVE_JANSSON] */

		if (result == 0) {
			if (!json_output) {
				d_printf("No locked files\n");
			}
		} else if (result < 0) {
			if (json_output) {
				fprintf(stderr, "locked file list truncated\n");
			}
			else {
				d_printf("locked file list truncated\n");
			}
		}

		if (!json_output) {
			d_printf("\n");
		}

		if (show_brl && !json_output) {
			brl_forall(print_brl, NULL);
		}
		#if HAVE_JANSSON
		else if (show_brl) {
			struct json_object brl_array = json_new_array();
			if (json_is_invalid(&brl_array)) {
				locking_end();
				fprintf(stderr, "Failed to create JSON array [brl_array]\n");
				ret = 1;
				goto done;
			}
			brl_forall(print_brl_json, &brl_array);
			if (json_add_object(&jsobj, "brl", &brl_array) < 0) {
				locking_end();
				fprintf(stderr, "Failed to add JSON array [brl_array]\n");
				ret = 1;
				goto done;
			}
		}
		#endif /* [HAVE_JANSSON] */

		locking_end();
	}

	if (show_notify) {
		if (!json_output) {
			notify_walk(msg_ctx, print_notify_rec, NULL);
		}
		#if HAVE_JANSSON
		else {
			struct json_object notify_array = json_new_array();
			if (json_is_invalid(&notify_array)) {
				locking_end();
				fprintf(stderr, "Failed to create JSON array [notify_array]\n");
				ret = 1;
				goto done;
			}
			notify_walk(msg_ctx, print_notify_rec_json, &notify_array);
			if (json_add_object(&jsobj, "notify", &notify_array) < 0) {
				locking_end();
				fprintf(stderr, "Failed to add JSON array [notify_array]\n");
				ret = 1;
				goto done;
			}
		}
		#endif /* [HAVE_JANSSON] */
	}

done:
	cmdline_messaging_context_free();
	poptFreeContext(pc);
	#ifdef HAVE_JANSSON
	if (json_output && !profile_only) {
		d_printf("%s\n", json_to_string(frame, &jsobj));
	}
	json_free(&jsobj);
	#endif /* [HAVE_JANSSON] */
	TALLOC_FREE(frame);
	return ret;
}
