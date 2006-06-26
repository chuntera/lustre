/* -*- MODE: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  fld/fld_internal.h
 *
 *  Copyright (C) 2006 Cluster File Systems, Inc.
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */
#ifndef _FLD_INTERNAL_H
#define _FLD_INTERNAL_H

#include <linux/types.h>

typedef __u64 fidseq_t;

struct fld_cache_entry {
        struct hlist_node  fce_list;
        mdsno_t            fce_mds;
        fidseq_t           fce_seq;
};

struct fld_cache_info {
        struct hlist_head *fci_hash;
        spinlock_t         fci_lock;
        int                fci_hash_mask;
};

enum fld_op {
        FLD_CREATE = 0,
        FLD_DELETE = 1,
        FLD_LOOKUP = 2
};

enum {
        FLD_HTABLE_BITS = 8,
        FLD_HTABLE_SIZE = (1 << FLD_HTABLE_BITS),
        FLD_HTABLE_MASK = FLD_HTABLE_SIZE - 1
};

extern struct lu_fld_hash fld_hash[3];
extern struct fld_cache_info *fld_cache;

#define FLD_SERVICE_WATCHDOG_TIMEOUT (obd_timeout * 1000)

int fld_index_handle_insert(struct lu_server_fld *fld,
                            const struct lu_context *ctx,
                            fidseq_t seq, mdsno_t mds);

int fld_index_handle_delete(struct lu_server_fld *fld,
                            const struct lu_context *ctx,
                            fidseq_t seq);

int fld_index_handle_lookup(struct lu_server_fld *fld,
                            const struct lu_context *ctx,
                            fidseq_t seq, mdsno_t *mds);

int fld_index_init(struct lu_server_fld *fld,
                   const struct lu_context *ctx);

void fld_index_fini(struct lu_server_fld *fld,
                    const struct lu_context *ctx);

#ifdef LPROCFS
extern struct lprocfs_vars fld_server_proc_list[];
extern struct lprocfs_vars fld_client_proc_list[];
#endif

#endif
