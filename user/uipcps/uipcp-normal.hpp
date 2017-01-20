/*
 * Core implementation of normal uipcps.
 *
 * Copyright (C) 2015-2016 Nextworks
 * Author: Vincenzo Maffione <v.maffione@gmail.com>
 *
 * This file is part of rlite.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __UIPCP_RIB_H__
#define __UIPCP_RIB_H__

#include <string>
#include <map>
#include <list>
#include <ctime>
#include <pthread.h>

#include "rlite/common.h"
#include "rlite/utils.h"
#include "rlite/uipcps-msg.h"
#include "rlite/cdap.hpp"
#include "rlite/cpputils.hpp"

#include "uipcp-normal-codecs.hpp"
#include "uipcp-container.h"

namespace obj_class {
    extern std::string adata;
    extern std::string dft;
    extern std::string neighbors;
    extern std::string enrollment;
    extern std::string status;
    extern std::string address;
    extern std::string lfdb; /* Lower Flow DB */
    extern std::string flows; /* Supported flows */
    extern std::string flow;
    extern std::string keepalive;
    extern std::string lowerflow;
    extern std::string addr_alloc_req;
    extern std::string addr_alloc_table;
};

namespace obj_name {
    extern std::string adata;
    extern std::string dft;
    extern std::string neighbors;
    extern std::string enrollment;
    extern std::string status;
    extern std::string address;
    extern std::string lfdb;
    extern std::string whatevercast;
    extern std::string flows;
    extern std::string keepalive;
    extern std::string lowerflow;
    extern std::string addr_alloc_table;
};

/* Time interval (in seconds) between two consecutive increments
 * of the age of LFDB entries. */
#define RL_AGE_INCR_INTERVAL    2

/* Max age (in seconds) for an LFDB entry not to be discarded. */
#define RL_AGE_MAX              120

/* Time interval (in seconds) between two consecutive periodic
 * RIB synchronizations. */
#define RL_NEIGH_SYNC_INTVAL           30

enum enroll_state_t {
    NEIGH_NONE = 0,

    NEIGH_ENROLLING,
    NEIGH_ENROLLED,

    NEIGH_STATE_LAST,
};

struct Neighbor;

/* Holds the information about an N-1 flow towards a neighbor IPCP. */
struct NeighFlow {
    Neighbor *neigh;
    std::string supp_dif;
    rl_port_t port_id;
    rl_ipcp_id_t lower_ipcp_id;
    int flow_fd; /* only used for close() */
    bool reliable;
    int upper_flow_fd; /* TODO currently unused */
    CDAPConn *conn;

    time_t last_activity;

    pthread_t enroll_th;
    enum enroll_state_t enroll_state;
    std::list<const CDAPMessage *> enroll_msgs;
    pthread_cond_t enroll_msgs_avail;
    pthread_cond_t enroll_stopped;
    bool enroll_rsrc_up; /* are resources allocated */

    int keepalive_tmrid;
    int pending_keepalive_reqs;

    NeighFlow(Neighbor *n, const std::string& supp_dif, unsigned int pid,
              int ffd, unsigned int lid);
    ~NeighFlow();

    void keepalive_tmr_start();
    void keepalive_tmr_stop();

    void enroll_state_set(enroll_state_t st);
    const CDAPMessage *next_enroll_msg();
    void enrollment_start(bool initiator);
    void enrollment_commit();
    void enrollment_cleanup();
    void enrollment_abort();

    int send_to_port_id(CDAPMessage *m, int invoke_id,
                        const UipcpObject *obj);
};

/* Holds the information about a neighbor IPCP. */
struct Neighbor {
    /* Backpointer to the RIB. */
    struct uipcp_rib *rib;

    /* Name of the neighbor. */
    std::string ipcp_name;

    /* Did we initiate the enrollment procedure towards this neighbor
     * or were we the target? */
    bool initiator;

    std::map<rl_port_t, NeighFlow *> flows;
    rl_port_t mgmt_port_id;

    /* Last time we received a keepalive response from this neighbor.
     * We don't consider requests, as timeout on responses. */
    time_t unheard_since;

    typedef int (Neighbor::*enroll_fsm_handler_t)(NeighFlow *nf,
                                                  const CDAPMessage *rm);

    Neighbor(struct uipcp_rib *rib, const std::string& name);
    bool operator==(const Neighbor& other) const
        { return ipcp_name == other.ipcp_name; }
    bool operator!=(const Neighbor& other) const
        { return !(*this == other); }
    ~Neighbor();

    const char *enroll_state_repr(enroll_state_t s) const;

    NeighFlow *mgmt_conn();
    const NeighFlow *mgmt_conn() const { return _mgmt_conn(); };
    bool has_mgmt_flow() const { return flows.size() > 0; }
    bool enrollment_complete() const;
    int alloc_flow(const char *supp_dif_name);

    /* Enrollment state machine handlers. */
    int none(NeighFlow *nf, const CDAPMessage *rm);
    int i_wait_connect_r(NeighFlow *nf, const CDAPMessage *rm);
    int s_wait_start(NeighFlow *nf, const CDAPMessage *rm);
    int i_wait_start_r(NeighFlow *nf, const CDAPMessage *rm);
    int i_wait_stop(NeighFlow *nf, const CDAPMessage *rm);
    int s_wait_stop_r(NeighFlow *nf, const CDAPMessage *rm);
    int i_wait_start(NeighFlow *nf, const CDAPMessage *rm);

    int s_lf_wait_start(NeighFlow *nf, const CDAPMessage *rm);
    int i_lf_wait_start_r(NeighFlow *nf, const CDAPMessage *rm);

    int fsm_enrolled(NeighFlow *nf, const CDAPMessage *rm);

    int neigh_sync_obj(const NeighFlow *nf, bool create,
                        const std::string& obj_class,
                        const std::string& obj_name,
                        const UipcpObject *obj_value) const;

    int neigh_sync_rib(NeighFlow *nf) const;

private:
    const NeighFlow *_mgmt_conn() const;
};

/* Shortest Path algorithm. */
class SPEngine {
public:
    SPEngine() {};
    int run(rl_addr_t, struct uipcp_rib *rib);

    /* The routing table computed by run(). */
    std::map<rl_addr_t, rl_addr_t> next_hops;

private:
    struct Edge {
        rl_addr_t to;
        unsigned int cost;

        Edge(rl_addr_t to_, unsigned int cost_) :
                            to(to_), cost(cost_) { }
    };

    struct Info {
        unsigned int dist;
        bool visited;
    };

    std::map<rl_addr_t, std::list<Edge> > graph;
    std::map<rl_addr_t, Info> info;
};

class ScopeLock {
public:
    ScopeLock(pthread_mutex_t& m) : mutex(m) {
        pthread_mutex_lock(&mutex);
    }
    ~ScopeLock() {
        pthread_mutex_unlock(&mutex);
    }

private:
    pthread_mutex_t& mutex;
};

struct uipcp_rib {
    /* Backpointer to parent data structure. */
    struct uipcp *uipcp;

    /* File descriptor used to receive and send mgmt PDUs. */
    int mgmtfd;

    /* RIB lock. */
    pthread_mutex_t lock;

    typedef int (uipcp_rib::*rib_handler_t)(const CDAPMessage *rm,
                                            NeighFlow *nf);
    std::map< std::string, rib_handler_t > handlers;

    /* Positive if this IPCP is enrolled to the DIF, zero otherwise.
     * When we allocate a flow towards a candidate neighbor, we don't
     * have to carry out the whole enrollment procedure if we are already
     * enrolled. */
    int enrolled;

    /* True if the name of this IPCP is registered to the IPCP itself.
     * Self-registration is used to receive N-flow allocation requests. */
    bool self_registered;
    bool self_registration_needed;

    /* IPCP address .*/
    rl_addr_t myaddr;

    /* Lower DIFs. */
    std::list< std::string > lower_difs;

    /* Neighbors. We keep track of all the NeighborCandidate objects seen,
     * even for candidates that have no lower DIF in common with us. This
     * is used to implement propagation of the CandidateNeighbors information,
     * so that all the IPCPs in the DIF know their potential candidate
     * neighbors.*/
    std::map< std::string, Neighbor* > neighbors;
    std::map< std::string, NeighborCandidate > neighbors_seen;
    std::set< std::string > neighbors_cand;

    /* Table used to carry on distributed address allocation.
     * It maps (address allocated) --> (requestor address). */
    std::map<rl_addr_t, AddrAllocRequest> addr_alloc_table;

    /* Directory Forwarding Table. */
    std::map< std::string, DFTEntry > dft;

    /* Lower Flow Database. */
    std::map< rl_addr_t, std::map<rl_addr_t, LowerFlow > > lfdb;

    SPEngine spe;

    /* Timer ID for LFDB synchronization with neighbors. */
    int sync_tmrid;

    /* For A-DATA messages. */
    InvokeIdMgr invoke_id_mgr;

    /* Supported flows. */
    std::map< std::string, FlowRequest > flow_reqs;
    std::map< unsigned int, FlowRequest > flow_reqs_tmp;

#ifdef RL_USE_QOS_CUBES
    /* Available QoS cubes. */
    std::map< std::string, struct rl_flow_config > qos_cubes;
#endif /* RL_USE_QOS_CUBES */

    /* Timer ID for age increment of LFDB entries. */
    int age_incr_tmrid;

    uipcp_rib(struct uipcp *_u);
    ~uipcp_rib();

    char *dump() const;

    int set_address(rl_addr_t address);
    Neighbor *get_neighbor(const std::string& neigh_name, bool create);
    int del_neighbor(const std::string& neigh_name);
    int dft_lookup(const std::string& appl_name, rl_addr_t& dstaddr) const;
    int dft_set(const std::string& appl_name, rl_addr_t remote_addr);
    void dft_update_address(rl_addr_t new_addr);
    int register_to_lower(int reg, std::string lower_dif);
    int appl_register(const struct rl_kmsg_appl_register *req);
    int flow_deallocated(struct rl_kmsg_flow_deallocated *req);
    rl_addr_t lookup_neighbor_address(const std::string& neigh_name) const;
    std::string lookup_neighbor_by_address(rl_addr_t address);
    int lookup_neigh_flow_by_port_id(rl_port_t port_id,
                                     NeighFlow **nfp);
    int fa_req(struct rl_kmsg_fa_req *req);
    int fa_resp(struct rl_kmsg_fa_resp *resp);
    int pduft_sync();
    rl_addr_t address_allocate();
    void neigh_flow_prune(NeighFlow *nf);

    const LowerFlow *lfdb_find(rl_addr_t local_addr,
                               rl_addr_t remote_addr) const {
        return _lfdb_find(local_addr, remote_addr);
    };
    LowerFlow *lfdb_find(rl_addr_t local_addr, rl_addr_t remote_addr);
    bool lfdb_add(const LowerFlow &lf);
    bool lfdb_del(rl_addr_t local_addr, rl_addr_t remote_addr);
    void lfdb_update_local(const std::string& neigh_name);

    int send_to_dst_addr(CDAPMessage *m, rl_addr_t dst_addr,
                         const UipcpObject *obj);
    int send_to_myself(CDAPMessage *m, const UipcpObject *obj);

    /* Synchronize with neighbors. */
    int neighs_sync_obj_excluding(const Neighbor *exclude, bool create,
                              const std::string& obj_class,
                              const std::string& obj_name,
                              const UipcpObject *obj_value) const;
    int neighs_sync_obj_all(bool create, const std::string& obj_class,
                        const std::string& obj_name,
                        const UipcpObject *obj_value) const;
    int neighs_refresh_lower_flows();

    /* Receive info from neighbors. */
    int cdap_dispatch(const CDAPMessage *rm, NeighFlow *nf);

    /* RIB handlers for received CDAP messages. */
    int dft_handler(const CDAPMessage *rm, NeighFlow *nf);
    int neighbors_handler(const CDAPMessage *rm, NeighFlow *nf);
    int lfdb_handler(const CDAPMessage *rm, NeighFlow *nf);
    int flows_handler(const CDAPMessage *rm, NeighFlow *nf);
    int keepalive_handler(const CDAPMessage *rm, NeighFlow *nf);
    int status_handler(const CDAPMessage *rm, NeighFlow *nf);
    int addr_alloc_table_handler(const CDAPMessage *rm, NeighFlow *nf);

    int flows_handler_create(const CDAPMessage *rm);
    int flows_handler_create_r(const CDAPMessage *rm);
    int flows_handler_delete(const CDAPMessage *rm);

private:
#ifdef RL_USE_QOS_CUBES
    int load_qos_cubes(const char *);
#endif /* RL_USE_QOS_CUBES */

    const LowerFlow *_lfdb_find(rl_addr_t local_addr,
                                rl_addr_t remote_addr) const;
    /* Id to be used with incoming flow allocation request. */
    uint32_t kevent_id_cnt;
};

static inline void
reliable_spec(struct rina_flow_spec *spec)
{
    rl_flow_spec_default(spec);
    spec->max_sdu_gap = 0;
    spec->in_order_delivery = 1;
    rina_flow_spec_fc_set(spec, 1);
}

static inline bool
is_reliable_spec(const struct rina_flow_spec *spec)
{
    return spec->max_sdu_gap == 0 &&
                spec->in_order_delivery == 1 &&
                    rina_flow_spec_fc_get(spec);
}

int normal_ipcp_enroll(struct uipcp *uipcp,
                       const struct rl_cmsg_ipcp_enroll *req,
                       int wait_for_completion);

void normal_trigger_tasks(struct uipcp *uipcp);

int mgmt_write_to_local_port(struct uipcp *uipcp, rl_port_t local_port,
                             void *buf, size_t buflen);

void age_incr_cb(struct uipcp *uipcp, void *arg);
void sync_timeout_cb(struct uipcp *uipcp, void *arg);

#define UIPCP_RIB(_u) ((uipcp_rib *)((_u)->priv))

#endif  /* __UIPCP_RIB_H__ */
