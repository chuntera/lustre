/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define DEBUG_SUBSYSTEM S_RPC

#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/lustre_net.h>

extern int request_in_callback(ptl_event_t *ev);

static int ptlrpc_check_event(struct ptlrpc_service *svc,
                              struct ptlrpc_thread *thread, ptl_event_t *event)
{
        int rc = 0;
        ENTRY;

        spin_lock(&svc->srv_lock);
        if (thread->t_flags & SVC_STOPPING)
                GOTO(out, rc = 1);

        LASSERT ((thread->t_flags & SVC_EVENT) == 0);

        if (ptl_is_valid_handle(&svc->srv_eq_h)) {
                int err;
                err = PtlEQGet(svc->srv_eq_h, event);

                if (err == PTL_OK) {
                        thread->t_flags |= SVC_EVENT;
                        GOTO(out, rc = 1);
                }

                if (err != PTL_EQ_EMPTY) {
                        CERROR("BUG: PtlEQGet returned %d\n", rc);
                        LBUG();
                }

                GOTO(out, rc = 0);
        }

        EXIT;
 out:
        spin_unlock(&svc->srv_lock);
        return rc;
}

struct ptlrpc_service *
ptlrpc_init_svc(__u32 bufsize, int nbuffs, int req_portal, int rep_portal,
                obd_uuid_t uuid, svc_handler_t handler, char *name)
{
        int err;
        int rc, i;
        struct ptlrpc_service *service;
        ENTRY;

        OBD_ALLOC(service, sizeof(*service));
        if (!service) {
                LBUG();
                RETURN(NULL);
        }

        service->srv_name = name;
        spin_lock_init(&service->srv_lock);
        INIT_LIST_HEAD(&service->srv_reqs);
        INIT_LIST_HEAD(&service->srv_threads);
        init_waitqueue_head(&service->srv_waitq);

        service->srv_buf_size = bufsize;
        service->srv_nbuffs = nbuffs;
        service->srv_rep_portal = rep_portal;
        service->srv_req_portal = req_portal;
        service->srv_handler = handler;

        err = kportal_uuid_to_peer(uuid, &service->srv_self);
        if (err) {
                CERROR("cannot get peer for uuid '%s'\n", uuid);
                OBD_FREE(service, sizeof(*service));
                RETURN(NULL);
        }

        /* NB We need exactly 1 event for each buffer we queue */
        rc = PtlEQAlloc(service->srv_self.peer_ni, service->srv_nbuffs,
                        request_in_callback, &(service->srv_eq_h));

        if (rc != PTL_OK) {
                CERROR("PtlEQAlloc failed: %d\n", rc);
                LBUG();
                OBD_FREE(service, sizeof(*service));
                RETURN(NULL);
        }

        OBD_ALLOC(service->srv_rqbds, service->srv_nbuffs *
                  sizeof(struct ptlrpc_request_buffer_desc));
        if (service->srv_rqbds == NULL) {
                CERROR("no memory\n");
                LBUG();
                GOTO(failed, NULL);
        }

        for (i = 0; i < service->srv_nbuffs; i++) {
                struct ptlrpc_request_buffer_desc *rqbd =&service->srv_rqbds[i];

                rqbd->rqbd_service = service;
                ptl_set_inv_handle (&rqbd->rqbd_me_h);
                OBD_ALLOC(rqbd->rqbd_buffer, service->srv_buf_size);
                if (rqbd->rqbd_buffer == NULL) {
                        CERROR("no memory\n");
                        LBUG();
                        GOTO(failed, NULL);
                }
                ptlrpc_link_svc_me(rqbd);
        }

        CDEBUG(D_NET, "Starting service listening on portal %d\n",
               service->srv_req_portal);

        RETURN(service);
failed:
        ptlrpc_unregister_service(service);
        return NULL;
}

static int handle_incoming_request(struct obd_device *obddev,
                                   struct ptlrpc_service *svc,
                                   ptl_event_t *event,
                                   struct ptlrpc_request *request)
{
        struct ptlrpc_request_buffer_desc *rqbd = event->mem_desc.user_ptr;
        int rc;

        /* FIXME: If we move to an event-driven model, we should put the request
         * on the stack of mds_handle instead. */
        LASSERT ((event->mem_desc.options & PTL_MD_IOV) == 0);
        LASSERT (rqbd->rqbd_service == svc);
        LASSERT (rqbd->rqbd_buffer == event->mem_desc.start);
        LASSERT (event->offset == 0);

        memset(request, 0, sizeof(*request));
        request->rq_svc = svc;
        request->rq_obd = obddev;
        request->rq_xid = event->match_bits;
        request->rq_reqmsg = event->mem_desc.start + event->offset;
        request->rq_reqlen = event->mem_desc.length;

        if (request->rq_reqlen < sizeof(struct lustre_msg)) {
                CERROR("incomplete request (%d): ptl %d from "LPX64" xid "LPD64"\n",
                       request->rq_reqlen, svc->srv_req_portal,
                       event->initiator.nid, request->rq_xid);
                spin_unlock(&svc->srv_lock);
                RETURN(-EINVAL);
        }

        if (NTOH__u32(request->rq_reqmsg->type) != PTL_RPC_MSG_REQUEST) {
                CERROR("wrong packet type received (type=%u)\n",
                       request->rq_reqmsg->type);
                LBUG();
                spin_unlock(&svc->srv_lock);
                RETURN(-EINVAL);
        }

        if (request->rq_reqmsg->magic != PTLRPC_MSG_MAGIC) {
                CERROR("wrong lustre_msg magic %d: ptl %d from "LPX64" xid "LPD64"\n",
                       request->rq_reqmsg->magic, svc->srv_req_portal,
                       event->initiator.nid, request->rq_xid);
                spin_unlock(&svc->srv_lock);
                RETURN(-EINVAL);
        }

        if (request->rq_reqmsg->version != PTLRPC_MSG_VERSION) {
                CERROR("wrong lustre_msg version %d: ptl %d from "LPX64" xid "LPD64"\n",
                       request->rq_reqmsg->version, svc->srv_req_portal,
                       event->initiator.nid, request->rq_xid);
                spin_unlock(&svc->srv_lock);
                RETURN(-EINVAL);
        }

        CDEBUG(D_NET, "got req "LPD64"\n", request->rq_xid);

        request->rq_peer.peer_nid = event->initiator.nid;
        /* FIXME: this NI should be the incoming NI.
         * We don't know how to find that from here. */
        request->rq_peer.peer_ni = svc->srv_self.peer_ni;

        request->rq_export = class_conn2export((struct lustre_handle *)
                                               request->rq_reqmsg);

        if (request->rq_export) {
                request->rq_connection = request->rq_export->exp_connection;
                ptlrpc_connection_addref(request->rq_connection);
        }

        spin_unlock(&svc->srv_lock);

        rc = svc->srv_handler(request);
        ptlrpc_put_connection(request->rq_connection);

        ptlrpc_link_svc_me (rqbd);
        return rc;
}

static int ptlrpc_main(void *arg)
{
        struct ptlrpc_svc_data *data = (struct ptlrpc_svc_data *)arg;
        struct obd_device *obddev = data->dev;
        struct ptlrpc_service *svc = data->svc;
        struct ptlrpc_thread *thread = data->thread;
        struct ptlrpc_request *request;
        ptl_event_t *event;
        int rc = 0;

        ENTRY;

        lock_kernel();
        daemonize();
        spin_lock_irq(&current->sigmask_lock);
        sigfillset(&current->blocked);
        recalc_sigpending(current);
        spin_unlock_irq(&current->sigmask_lock);

        strcpy(current->comm, data->name);
        unlock_kernel();

        OBD_ALLOC(event, sizeof(*event));
        if (!event)
                GOTO(out, rc = -ENOMEM);
        OBD_ALLOC(request, sizeof(*request));
        if (!request)
                GOTO(out_event, rc = -ENOMEM);

        /* Record that the thread is running */
        thread->t_flags = SVC_RUNNING;
        wake_up(&thread->t_ctl_waitq);

        /* XXX maintain a list of all managed devices: insert here */

        /* And now, loop forever on requests */
        while (1) {
                wait_event(svc->srv_waitq,
                           ptlrpc_check_event(svc, thread, event));

                spin_lock(&svc->srv_lock);

                if (thread->t_flags & SVC_STOPPING) {
                        thread->t_flags &= ~SVC_STOPPING;
                        spin_unlock(&svc->srv_lock);
                        EXIT;
                        break;
                }

                if (thread->t_flags & SVC_EVENT) {
                        LASSERT (event->sequence != 0);
                        rc = handle_incoming_request(obddev, svc, event,
                                                     request);
                        thread->t_flags &= ~SVC_EVENT;
                        continue;
                }

                CERROR("unknown break in service");
                spin_unlock(&svc->srv_lock);
                EXIT;
                break;
        }

        OBD_FREE(request, sizeof(*request));
out_event:
        OBD_FREE(event, sizeof(*event));
out:
        thread->t_flags = SVC_STOPPED;
        wake_up(&thread->t_ctl_waitq);

        CDEBUG(D_NET, "service thread exiting, process %d: rc = %d\n",
               current->pid, rc);
        return rc;
}

static void ptlrpc_stop_thread(struct ptlrpc_service *svc,
                               struct ptlrpc_thread *thread)
{
        spin_lock(&svc->srv_lock);
        thread->t_flags = SVC_STOPPING;
        spin_unlock(&svc->srv_lock);

        wake_up(&svc->srv_waitq);
        wait_event(thread->t_ctl_waitq, (thread->t_flags & SVC_STOPPED));
}

void ptlrpc_stop_all_threads(struct ptlrpc_service *svc)
{
        spin_lock(&svc->srv_lock);
        while (!list_empty(&svc->srv_threads)) {
                struct ptlrpc_thread *thread;
                thread = list_entry(svc->srv_threads.next, struct ptlrpc_thread,
                                    t_link);
                spin_unlock(&svc->srv_lock);
                ptlrpc_stop_thread(svc, thread);
                spin_lock(&svc->srv_lock);
                list_del(&thread->t_link);
                OBD_FREE(thread, sizeof(*thread));
        }
        spin_unlock(&svc->srv_lock);
}

int ptlrpc_start_thread(struct obd_device *dev, struct ptlrpc_service *svc,
                        char *name)
{
        struct ptlrpc_svc_data d;
        struct ptlrpc_thread *thread;
        int rc;
        ENTRY;

        OBD_ALLOC(thread, sizeof(*thread));
        if (thread == NULL) {
                LBUG();
                RETURN(-ENOMEM);
        }
        init_waitqueue_head(&thread->t_ctl_waitq);

        d.dev = dev;
        d.svc = svc;
        d.name = name;
        d.thread = thread;

        spin_lock(&svc->srv_lock);
        list_add(&thread->t_link, &svc->srv_threads);
        spin_unlock(&svc->srv_lock);

        /* XXX should we really be cloning open file handles here? */
        rc = kernel_thread(ptlrpc_main, (void *) &d,
                           CLONE_VM | CLONE_FS | CLONE_FILES);
        if (rc < 0) {
                CERROR("cannot start thread\n");
                OBD_FREE(thread, sizeof(*thread));
                RETURN(rc);
        }
        wait_event(thread->t_ctl_waitq, thread->t_flags & SVC_RUNNING);

        RETURN(0);
}

int ptlrpc_unregister_service(struct ptlrpc_service *service)
{
        int rc, i;

        /* NB service->srv_nbuffs gets set before we attempt (and possibly
         * fail) to allocate srv_rqbds.
         */
        if (service->srv_rqbds != NULL) {
                for (i = 0; i < service->srv_nbuffs; i++) {
                        struct ptlrpc_request_buffer_desc *rqbd =
                                &service->srv_rqbds[i];

                        if (rqbd->rqbd_buffer == NULL) /* no buffer allocated */
                                continue;             /* => never initialised */

                        /* Buffer allocated => got linked */
                        LASSERT (ptl_is_valid_handle (&rqbd->rqbd_me_h));

                        rc = PtlMEUnlink(rqbd->rqbd_me_h);
                        if (rc)
                                CERROR("PtlMEUnlink failed: %d\n", rc);

                        OBD_FREE(rqbd->rqbd_buffer, service->srv_buf_size);
                }

                OBD_FREE(service->srv_rqbds, service->srv_nbuffs *
                         sizeof (struct ptlrpc_request_buffer_desc));
        }

        rc = PtlEQFree(service->srv_eq_h);
        if (rc)
                CERROR("PtlEQFree failed: %d\n", rc);

        if (!list_empty(&service->srv_reqs)) {
                // XXX reply with errors and clean up
                CERROR("Request list not empty!\n");
                rc = -EBUSY;
        }

        OBD_FREE(service, sizeof(*service));
        if (rc)
                LBUG();
        return rc;
}
