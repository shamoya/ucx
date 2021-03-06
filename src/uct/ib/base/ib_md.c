/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2016.  ALL RIGHTS RESERVED.
 * Copyright (C) The University of Tennessee and The University
 *               of Tennessee Research Foundation. 2016. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ib_md.h"
#include "ib_device.h"

#include <ucs/arch/atomic.h>
#include <pthread.h>

#define UCT_IB_MD_PREFIX         "ib"
#define UCT_IB_MEM_ACCESS_FLAGS  (IBV_ACCESS_LOCAL_WRITE | \
                                  IBV_ACCESS_REMOTE_WRITE | \
                                  IBV_ACCESS_REMOTE_READ | \
                                  IBV_ACCESS_REMOTE_ATOMIC)

static ucs_config_field_t uct_ib_md_config_table[] = {
  {"", "", NULL,
   ucs_offsetof(uct_ib_md_config_t, super), UCS_CONFIG_TYPE_TABLE(uct_md_config_table)},

  {"RCACHE", "try", "Enable using memory registration cache",
   ucs_offsetof(uct_ib_md_config_t, rcache.enable), UCS_CONFIG_TYPE_TERNARY},

  {"RCACHE_MEM_PRIO", "1000", "Registration cache memory event priority",
   ucs_offsetof(uct_ib_md_config_t, rcache.event_prio), UCS_CONFIG_TYPE_UINT},

  {"RCACHE_OVERHEAD", "90ns", "Registration cache lookup overhead",
   ucs_offsetof(uct_ib_md_config_t, rcache.overhead), UCS_CONFIG_TYPE_TIME},

  {"MEM_REG_OVERHEAD", "16us", "Memory registration overhead", /* TODO take default from device */
   ucs_offsetof(uct_ib_md_config_t, uc_reg_cost.overhead), UCS_CONFIG_TYPE_TIME},

  {"MEM_REG_GROWTH", "0.06ns", "Memory registration growth rate", /* TODO take default from device */
   ucs_offsetof(uct_ib_md_config_t, uc_reg_cost.growth), UCS_CONFIG_TYPE_TIME},

  {"FORK_INIT", "try",
   "Initialize a fork-safe IB library with ibv_fork_init().",
   ucs_offsetof(uct_ib_md_config_t, fork_init), UCS_CONFIG_TYPE_TERNARY},

  {"ETH_PAUSE_ON", "n",
   "Whether or not 'Pause Frame' is enabled on an Ethernet network.\n"
   "Pause frame is a mechanism for temporarily stopping the transmission of data to\n"
   "ensure zero loss under congestion on Ethernet family computer networks.\n"
   "This parameter, if set to 'no', will disqualify IB transports that may not perform\n"
   "well on a lossy fabric when working with RoCE.",
   ucs_offsetof(uct_ib_md_config_t, eth_pause), UCS_CONFIG_TYPE_BOOL},

  {"ODP_MAX_SIZE", "auto",
   "Maximal memory region size to enable on-demand-paging (ODP) for. 0 - disable.\n",
   ucs_offsetof(uct_ib_md_config_t, odp_max_size), UCS_CONFIG_TYPE_MEMUNITS},

  {"PREFETCH_MR", "y",
   "Prefetch memory regions created with NONBLOCKING flag.\n",
   ucs_offsetof(uct_ib_md_config_t, prefetch_mr), UCS_CONFIG_TYPE_BOOL},

  {NULL}
};

#if ENABLE_STATS
static ucs_stats_class_t uct_ib_md_stats_class = {
    .name           = "",
    .num_counters   = UCT_IB_MD_STAT_LAST,
    .counter_names = {
        [UCT_IB_MD_STAT_MEM_ALLOC]   = "mem_alloc",
        [UCT_IB_MD_STAT_MEM_REG]     = "mem_reg"
    }
};
#endif

static ucs_status_t uct_ib_md_query(uct_md_h uct_md, uct_md_attr_t *md_attr)
{
    uct_ib_md_t *md = ucs_derived_of(uct_md, uct_ib_md_t);

    md_attr->cap.max_alloc = ULONG_MAX; /* TODO query device */
    md_attr->cap.max_reg   = ULONG_MAX; /* TODO query device */
    md_attr->cap.flags     = UCT_MD_FLAG_REG;
    md_attr->rkey_packed_size = sizeof(uint64_t);

    if (IBV_EXP_HAVE_CONTIG_PAGES(&md->dev.dev_attr)) {
        md_attr->cap.flags |= UCT_MD_FLAG_ALLOC;
    }

    md_attr->reg_cost      = md->reg_cost;
    md_attr->local_cpus    = md->dev.local_cpus;
    return UCS_OK;
}

static ucs_status_t uct_ib_md_umr_qp_create(uct_ib_md_t *md)
{
#if HAVE_EXP_UMR
    struct ibv_exp_qp_init_attr qp_init_attr;
    struct ibv_qp_attr qp_attr;
    uint8_t port_num;
    int ret;
    uct_ib_device_t *ibdev;
    struct ibv_exp_port_attr *port_attr;

    ibdev = &md->dev;

    if (!(ibdev->dev_attr.exp_device_cap_flags & IBV_EXP_DEVICE_UMR)) {
        return UCS_ERR_UNSUPPORTED;
    }

    /* TODO: fix port selection. It looks like active port should be used */
    port_num = ibdev->first_port;
    port_attr = uct_ib_device_port_attr(ibdev, port_num);

    memset(&qp_init_attr, 0, sizeof(qp_init_attr));

    md->umr_cq = ibv_create_cq(ibdev->ibv_context, 1, NULL, NULL, 0);
    if (md->umr_cq == NULL) {
        ucs_error("failed to create UMR CQ: %m");
        goto err;
    }

    qp_init_attr.qp_type             = IBV_QPT_RC;
    qp_init_attr.send_cq             = md->umr_cq;
    qp_init_attr.recv_cq             = md->umr_cq;
    qp_init_attr.cap.max_inline_data = 0;
    qp_init_attr.cap.max_recv_sge    = 1;
    qp_init_attr.cap.max_send_sge    = 1;
    qp_init_attr.srq                 = NULL;
    qp_init_attr.cap.max_recv_wr     = 16;
    qp_init_attr.cap.max_send_wr     = 16;
    qp_init_attr.pd                  = md->pd;
    qp_init_attr.comp_mask           = IBV_EXP_QP_INIT_ATTR_PD|IBV_EXP_QP_INIT_ATTR_MAX_INL_KLMS;
    qp_init_attr.max_inl_recv        = 0;
#if (HAVE_IBV_EXP_QP_CREATE_UMR_CAPS || HAVE_EXP_UMR_NEW_API)
    qp_init_attr.max_inl_send_klms   = ibdev->dev_attr.umr_caps.max_send_wqe_inline_klms;
#else
    qp_init_attr.max_inl_send_klms   = ibdev->dev_attr.max_send_wqe_inline_klms;
#endif

#if HAVE_IBV_EXP_QP_CREATE_UMR
    qp_init_attr.comp_mask          |= IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
    qp_init_attr.exp_create_flags    = IBV_EXP_QP_CREATE_UMR;
#endif

    md->umr_qp = ibv_exp_create_qp(ibdev->ibv_context, &qp_init_attr);
    if (md->umr_qp == NULL) {
        ucs_error("failed to create UMR QP: %m");
        goto err_destroy_cq;
    }

    memset(&qp_attr, 0, sizeof(qp_attr));

    /* Modify QP to INIT state */
    qp_attr.qp_state                 = IBV_QPS_INIT;
    qp_attr.pkey_index               = 0;
    qp_attr.port_num                 = port_num;
    qp_attr.qp_access_flags          = UCT_IB_MEM_ACCESS_FLAGS;
    ret = ibv_modify_qp(md->umr_qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) {
        ucs_error("Failed to modify UMR QP to INIT: %m");
        goto err_destroy_qp;
    }

    /* Modify to RTR */
    qp_attr.qp_state                 = IBV_QPS_RTR;
    qp_attr.dest_qp_num              = md->umr_qp->qp_num;

    memset(&qp_attr.ah_attr, 0, sizeof(qp_attr.ah_attr));
    qp_attr.ah_attr.port_num         = port_num;
    qp_attr.ah_attr.dlid             = port_attr->lid;
    qp_attr.ah_attr.is_global        = 1;
    if (uct_ib_device_query_gid(ibdev, port_num, 0, &qp_attr.ah_attr.grh.dgid) != UCS_OK) {
        goto err_destroy_qp;
    }
    qp_attr.rq_psn                   = 0;
    qp_attr.path_mtu                 = IBV_MTU_512;
    qp_attr.min_rnr_timer            = 7;
    qp_attr.max_dest_rd_atomic       = 1;
    ret = ibv_modify_qp(md->umr_qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret) {
        ucs_error("Failed to modify UMR QP to RTR: %m");
        goto err_destroy_qp;
    }

    /* Modify to RTS */
    qp_attr.qp_state                 = IBV_QPS_RTS;
    qp_attr.sq_psn                   = 0;
    qp_attr.timeout                  = 7;
    qp_attr.rnr_retry                = 7;
    qp_attr.retry_cnt                = 7;
    qp_attr.max_rd_atomic            = 1;
    ret = ibv_modify_qp(md->umr_qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                        IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) {
        ucs_error("Failed to modify UMR QP to RTS: %m");
        goto err_destroy_qp;
    }
    return UCS_OK;

err_destroy_qp:
    ibv_destroy_qp(md->umr_qp);
err_destroy_cq:
    ibv_destroy_cq(md->umr_cq);
err:
    return UCS_ERR_IO_ERROR;
#else
    return UCS_ERR_UNSUPPORTED;
#endif
}

static void uct_ib_md_umr_qp_destroy(uct_ib_md_t *md)
{
#if HAVE_EXP_UMR
    if (md->umr_qp != NULL) {
        ibv_destroy_qp(md->umr_qp);
    }
    if (md->umr_cq != NULL) {
        ibv_destroy_cq(md->umr_cq);
    }
#endif
}

uint8_t  uct_ib_md_umr_id(uct_ib_md_t *md)
{
#if HAVE_EXP_UMR
    if ((md->umr_qp == NULL) || (md->umr_cq == NULL)) {
        return 0;
    }
    /* Generate umr id. We want umrs for same virtual adresses to have different
     * ids across proceses.
     *
     * Usually parallel processes running on the same node as part of a single
     * job will have consequitive pids. For example mpi ranks, slurm spawned tasks...
     */
    return getpid() % 256;
#else
    return 0;
#endif
}

static ucs_status_t uct_ib_md_reg_mr(uct_ib_md_t *md, void *address,
                                     size_t length, uint64_t exp_access,
                                     struct ibv_mr **mr_p)
{
    struct ibv_mr *mr;

    if (exp_access) {
#if HAVE_DECL_IBV_EXP_REG_MR
        struct ibv_exp_reg_mr_in in;

        memset(&in, 0, sizeof(in));
        in.pd           = md->pd;
        in.addr         = address;
        in.length       = length;
        in.exp_access   = UCT_IB_MEM_ACCESS_FLAGS | exp_access;

        mr = ibv_exp_reg_mr(&in);
        if (mr == NULL) {
            ucs_error("ibv_exp_reg_mr(address=%p, length=%Zu, exp_access=0x%lx) failed: %m",
                      in.addr, in.length, in.exp_access);
            return UCS_ERR_IO_ERROR;
        }
#else
        return UCS_ERR_UNSUPPORTED;
#endif
    } else {
        mr = ibv_reg_mr(md->pd, address, length, UCT_IB_MEM_ACCESS_FLAGS);
        if (mr == NULL) {
            ucs_error("ibv_reg_mr(address=%p, length=%Zu, access=0x%x) failed: %m",
                      address, length, UCT_IB_MEM_ACCESS_FLAGS);
            return UCS_ERR_IO_ERROR;
        }
    }

    *mr_p = mr;
    return UCS_OK;
}

static UCS_F_MAYBE_UNUSED
struct ibv_mr *uct_ib_md_create_umr(uct_ib_md_t *md, struct ibv_mr *mr)
{
#if HAVE_EXP_UMR
    struct ibv_exp_mem_region mem_reg;
    struct ibv_exp_send_wr wr, *bad_wr;
    struct ibv_exp_create_mr_in mrin;
    struct ibv_mr *umr;
    struct ibv_wc wc;
    int ret;
    size_t offset;

    if ((md->umr_qp == NULL) || (md->umr_cq == NULL)) {
        return NULL;
    }

    offset = uct_ib_md_umr_offset(uct_ib_md_umr_id(md));
    /* Create memory key */
    memset(&mrin, 0, sizeof(mrin));
    mrin.pd                       = md->pd;

#ifdef HAVE_EXP_UMR_NEW_API
    mrin.attr.create_flags        = IBV_EXP_MR_INDIRECT_KLMS;
    mrin.attr.exp_access_flags    = UCT_IB_MEM_ACCESS_FLAGS;
    mrin.attr.max_klm_list_size   = 1;
#else
    mrin.attr.create_flags        = IBV_MR_NONCONTIG_MEM;
    mrin.attr.access_flags        = UCT_IB_MEM_ACCESS_FLAGS;
    mrin.attr.max_reg_descriptors = 1;
#endif

    umr = ibv_exp_create_mr(&mrin);
    if (!umr) {
        ucs_error("Failed to create modified_mr: %m");
        goto err;
    }

    /* Fill memory list and UMR */
    memset(&wr, 0, sizeof(wr));
    memset(&mem_reg, 0, sizeof(mem_reg));

    mem_reg.base_addr                              = (uintptr_t) mr->addr;
    mem_reg.length                                 = mr->length;

#ifdef HAVE_EXP_UMR_NEW_API
    mem_reg.mr                                     = mr;

    wr.ext_op.umr.umr_type                         = IBV_EXP_UMR_MR_LIST;
    wr.ext_op.umr.mem_list.mem_reg_list            = &mem_reg;
    wr.ext_op.umr.exp_access                       = UCT_IB_MEM_ACCESS_FLAGS;
    wr.ext_op.umr.modified_mr                      = umr;
    wr.ext_op.umr.base_addr                        = (uint64_t) (uintptr_t) mr->addr + offset;

    wr.ext_op.umr.num_mrs                          = 1;
#else
    mem_reg.m_key                                  = mr;

    wr.ext_op.umr.memory_key.mkey_type             = IBV_EXP_UMR_MEM_LAYOUT_NONCONTIG;
    wr.ext_op.umr.memory_key.mem_list.mem_reg_list = &mem_reg;
    wr.ext_op.umr.memory_key.access                = UCT_IB_MEM_ACCESS_FLAGS;
    wr.ext_op.umr.memory_key.modified_mr           = umr;
    wr.ext_op.umr.memory_key.region_base_addr      = mr->addr + offset;

    wr.num_sge                                     = 1;
#endif

    wr.exp_opcode                                  = IBV_EXP_WR_UMR_FILL;
    wr.exp_send_flags                              = IBV_EXP_SEND_INLINE | IBV_EXP_SEND_SIGNALED;

    /* Post UMR */
    ret = ibv_exp_post_send(md->umr_qp, &wr, &bad_wr);
    if (ret) {
        ucs_error("ibv_exp_post_send(UMR_FILL) failed: %m");
        goto err_free_umr;
    }

    /* Wait for send UMR completion */
    for (;;) {
        ret = ibv_poll_cq(md->umr_cq, 1, &wc);
        if (ret < 0) {
            ucs_error("ibv_exp_poll_cq(umr_cq) failed: %m");
            goto err_free_umr;
        }
        if (ret == 1) {
            if (wc.status != IBV_WC_SUCCESS) {
                ucs_error("UMR_FILL completed with error: %s vendor_err %d",
                          ibv_wc_status_str(wc.status), wc.vendor_err);
                goto err_free_umr;
            }
            break;
        }
    }

    ucs_trace("UMR registered memory %p..%p offset 0x%x on %s lkey 0x%x rkey 0x%x",
              mr->addr, mr->addr + mr->length, (unsigned)offset, uct_ib_device_name(&md->dev), umr->lkey,
              umr->rkey);
    return umr;

err_free_umr:
    ibv_dereg_mr(umr);
err:
#endif
    return NULL;
}


static ucs_status_t uct_ib_dereg_mr(struct ibv_mr *mr)
{
    int ret;

    ret = ibv_dereg_mr(mr);
    if (ret != 0) {
        ucs_error("ibv_dereg_mr() failed: %m");
        return UCS_ERR_IO_ERROR;
    }

    return UCS_OK;
}

static ucs_status_t uct_ib_memh_dereg(uct_ib_mem_t *memh)
{
    ucs_status_t s1, s2;

    s1 = s2 = UCS_OK;
    if (memh->umr != NULL) {
        s2 = uct_ib_dereg_mr(memh->umr);
    }
    if (memh->mr != NULL) {
        s1 = uct_ib_dereg_mr(memh->mr);
    }
    return (s1 != UCS_OK) ? s1 : s2;
}

static void uct_ib_memh_free(uct_ib_mem_t *memh)
{
    ucs_free(memh);
}

static uct_ib_mem_t *uct_ib_memh_alloc()
{
    return ucs_calloc(1, sizeof(uct_ib_mem_t), "ib_memh");
}

static uint64_t uct_ib_md_access_flags(uct_ib_md_t *md, unsigned flags,
                                       size_t length)
{
    uint64_t exp_access = 0;

    if ((flags & UCT_MD_MEM_FLAG_NONBLOCK) && (length > 0) &&
        (length <= md->odp_max_size))
    {
        exp_access |= IBV_EXP_ACCESS_ON_DEMAND;
    }

    return exp_access;
}

static ucs_status_t uct_ib_mem_prefetch(uct_ib_md_t *md, uct_ib_mem_t *memh,
                                        uint64_t mr_access_flags)
{
#if HAVE_DECL_IBV_EXP_PREFETCH_MR
    struct ibv_exp_prefetch_attr attr;
    int ret;

    if ((mr_access_flags & IBV_EXP_ACCESS_ON_DEMAND) && md->prefetch_mr) {
        attr.flags     = IBV_EXP_PREFETCH_WRITE_ACCESS;
        attr.addr      = memh->mr->addr;
        attr.length    = memh->mr->length;
        attr.comp_mask = 0;

        ret = ibv_exp_prefetch_mr(memh->mr, &attr);
        if (ret) {
            ucs_error("ibv_exp_prefetch_mr(addr=%p length=%zu) returned %d: %m",
                      attr.addr, attr.length, ret);
            return UCS_ERR_IO_ERROR;
        }
    }
#endif

    return UCS_OK;
}

static ucs_status_t uct_ib_mem_alloc(uct_md_h uct_md, size_t *length_p,
                                     void **address_p, unsigned flags,
                                     uct_mem_h *memh_p UCS_MEMTRACK_ARG)
{
#if HAVE_DECL_IBV_EXP_ACCESS_ALLOCATE_MR
    uct_ib_md_t *md = ucs_derived_of(uct_md, uct_ib_md_t);
    ucs_status_t status;
    uint64_t exp_access;
    uct_ib_mem_t *memh;
    size_t length;

    memh = uct_ib_memh_alloc();
    if (memh == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err;
    }

    length     = ucs_memtrack_adjust_alloc_size(*length_p);
    exp_access = uct_ib_md_access_flags(md, flags, length) |
                 IBV_EXP_ACCESS_ALLOCATE_MR;
    status = uct_ib_md_reg_mr(md, NULL, length, exp_access, &memh->mr);
    if (status != UCS_OK) {
        goto err_free_memh;
    }

    memh->lkey = memh->mr->lkey;
    ucs_trace("allocated memory %p..%p on %s lkey 0x%x rkey 0x%x",
              memh->mr->addr, memh->mr->addr + memh->mr->length, uct_ib_device_name(&md->dev),
              memh->mr->lkey, memh->mr->rkey);

    memh->umr = uct_ib_md_create_umr(md, memh->mr);
#if HAVE_EXP_UMR
    if ((memh->umr) == NULL && (md->umr_qp)) {
        ibv_dereg_mr(memh->mr);
        status = UCS_ERR_IO_ERROR;
        goto err_free_memh;
    }
#endif

    uct_ib_mem_prefetch(md, memh, exp_access);

    UCS_STATS_UPDATE_COUNTER(md->stats, UCT_IB_MD_STAT_MEM_ALLOC, +1);
    *address_p = memh->mr->addr;
    *length_p  = memh->mr->length;
    *memh_p    = memh;
    ucs_memtrack_allocated(address_p, length_p UCS_MEMTRACK_VAL);

    return UCS_OK;
err_free_memh:
    uct_ib_memh_free(memh);
err:
    return status;
#else
    return UCS_ERR_UNSUPPORTED;
#endif
}

static ucs_status_t uct_ib_mem_free(uct_md_h md, uct_mem_h memh)
{
    uct_ib_mem_t *ib_memh = memh;
    ucs_status_t status;

    ucs_memtrack_releasing_adjusted(ib_memh->mr->addr);

    status = uct_ib_memh_dereg(memh);
    if (status != UCS_OK) {
        return status;
    }

    uct_ib_memh_free(ib_memh);
    return UCS_OK;
}

static ucs_status_t uct_ib_mem_reg_internal(uct_md_h uct_md, void *address,
                                            size_t length, unsigned flags,
                                            uct_ib_mem_t *memh)
{
    uct_ib_md_t *md = ucs_derived_of(uct_md, uct_ib_md_t);
    ucs_status_t status;
    uint64_t exp_access;

    exp_access = uct_ib_md_access_flags(md, flags, length);
    status = uct_ib_md_reg_mr(md, address, length, exp_access, &memh->mr);
    if (status != UCS_OK) {
        return status;
    }

    ucs_trace("registered memory %p..%p on %s lkey 0x%x rkey 0x%x",
              address, address + length, uct_ib_device_name(&md->dev),
              memh->mr->lkey, memh->mr->rkey);

    memh->lkey = memh->mr->lkey;

    memh->umr = uct_ib_md_create_umr(md, memh->mr);
#if HAVE_EXP_UMR
    if (memh->umr == NULL && md->umr_qp) {
        ibv_dereg_mr(memh->mr);
        return UCS_ERR_IO_ERROR;
    }
#endif

    uct_ib_mem_prefetch(md, memh, exp_access);

    UCS_STATS_UPDATE_COUNTER(md->stats, UCT_IB_MD_STAT_MEM_REG, +1);
    return UCS_OK;
}

static ucs_status_t uct_ib_mem_reg(uct_md_h uct_md, void *address, size_t length,
                                   unsigned flags, uct_mem_h *memh_p)
{
    ucs_status_t status;
    uct_ib_mem_t *memh;

    memh = uct_ib_memh_alloc();
    if (memh == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    status = uct_ib_mem_reg_internal(uct_md, address, length, flags, memh);
    if (status != UCS_OK) {
        uct_ib_memh_free(memh);
        return status;
    }
    *memh_p = memh;

    return UCS_OK;
}

static ucs_status_t uct_ib_mem_dereg_internal(uct_ib_mem_t *memh)
{
    return uct_ib_memh_dereg(memh);
}

static ucs_status_t uct_ib_mem_dereg(uct_md_h uct_md, uct_mem_h memh)
{
    ucs_status_t status;
    uct_ib_mem_t *ib_memh = memh;

    status = uct_ib_mem_dereg_internal(ib_memh);
    uct_ib_memh_free(ib_memh);
    return status;
}

static ucs_status_t uct_ib_mkey_pack(uct_md_h md, uct_mem_h memh,
                                     void *rkey_buffer)
{
    uct_ib_mem_t *ib_memh = memh;
    uint64_t *rkey_p = rkey_buffer;
    uint32_t umr_key;

    *rkey_p = ib_memh->mr->rkey;
    umr_key = ib_memh->umr != NULL ? ib_memh->umr->rkey : ib_memh->mr->rkey;
    *rkey_p |= (((uint64_t)umr_key) << 32);

    ucs_trace("packed rkey: umr=0x%x mr=0x%x",
              uct_ib_md_umr_rkey(*rkey_p), uct_ib_md_direct_rkey(*rkey_p));
    return UCS_OK;
}

static ucs_status_t uct_ib_rkey_unpack(uct_md_component_t *mdc,
                                       const void *rkey_buffer, uct_rkey_t *rkey_p,
                                       void **handle_p)
{
    uint64_t ib_rkey = *(const uint64_t*)rkey_buffer;

    *rkey_p   = ib_rkey;
    *handle_p = NULL;
    ucs_trace("unpacked rkey: 0x%llx umr=0x%x mr=0x%x",
              (unsigned long long)ib_rkey,
              uct_ib_md_umr_rkey(ib_rkey), uct_ib_md_direct_rkey(ib_rkey));
    return UCS_OK;
}

static void uct_ib_md_close(uct_md_h md);

static uct_md_ops_t uct_ib_md_ops = {
    .close        = uct_ib_md_close,
    .query        = uct_ib_md_query,
    .mem_alloc    = uct_ib_mem_alloc,
    .mem_free     = uct_ib_mem_free,
    .mem_reg      = uct_ib_mem_reg,
    .mem_dereg    = uct_ib_mem_dereg,
    .mkey_pack    = uct_ib_mkey_pack,
};

static inline uct_ib_rcache_region_t* uct_ib_rache_region_from_memh(uct_mem_h memh)
{
    return ucs_container_of(memh, uct_ib_rcache_region_t, memh);
}

static ucs_status_t uct_ib_mem_rcache_reg(uct_md_h uct_md, void *address,
                                          size_t length, unsigned flags,
                                          uct_mem_h *memh_p)
{
    uct_ib_md_t *md = ucs_derived_of(uct_md, uct_ib_md_t);
    ucs_rcache_region_t *rregion;
    ucs_status_t status;

    status = ucs_rcache_get(md->rcache, address, length, PROT_READ|PROT_WRITE,
                            &flags, &rregion);
    if (status != UCS_OK) {
        return status;
    }

    ucs_assert(rregion->refcount > 0);
    *memh_p = &ucs_derived_of(rregion, uct_ib_rcache_region_t)->memh;
    return UCS_OK;
}

static ucs_status_t uct_ib_mem_rcache_dereg(uct_md_h uct_md, uct_mem_h memh)
{
    uct_ib_md_t *md = ucs_derived_of(uct_md, uct_ib_md_t);
    uct_ib_rcache_region_t *region = uct_ib_rache_region_from_memh(memh);

    ucs_rcache_region_put(md->rcache, &region->super);
    return UCS_OK;
}

static uct_md_ops_t uct_ib_md_rcache_ops = {
    .close        = uct_ib_md_close,
    .query        = uct_ib_md_query,
    .mem_alloc    = uct_ib_mem_alloc,
    .mem_free     = uct_ib_mem_free,
    .mem_reg      = uct_ib_mem_rcache_reg,
    .mem_dereg    = uct_ib_mem_rcache_dereg,
    .mkey_pack    = uct_ib_mkey_pack,
};


static ucs_status_t uct_ib_rcache_mem_reg_cb(void *context, ucs_rcache_t *rcache,
                                             void *arg, ucs_rcache_region_t *rregion)
{
    uct_ib_rcache_region_t *region = ucs_derived_of(rregion, uct_ib_rcache_region_t);
    uct_ib_md_t *md = context;
    int *flags = arg;
    ucs_status_t status;

    status = uct_ib_mem_reg_internal(&md->super, (void*)region->super.super.start,
                                     region->super.super.end - region->super.super.start,
                                     *flags, &region->memh);
    if (status != UCS_OK) {
        return status;
    }

    return UCS_OK;
}

static void uct_ib_rcache_mem_dereg_cb(void *context, ucs_rcache_t *rcache,
                                       ucs_rcache_region_t *rregion)
{
    uct_ib_rcache_region_t *region = ucs_derived_of(rregion, uct_ib_rcache_region_t);

    (void)uct_ib_mem_dereg_internal(&region->memh);
}

static void uct_ib_rcache_dump_region_cb(void *context, ucs_rcache_t *rcache,
                                         ucs_rcache_region_t *rregion, char *buf,
                                         size_t max)
{
    uct_ib_rcache_region_t *region = ucs_derived_of(rregion, uct_ib_rcache_region_t);
    uct_ib_mem_t *memh = &region->memh;

    snprintf(buf, max, "lkey 0x%x rkey 0x%x umr: lkey 0x%x rkey 0x%x",
             memh->mr->lkey, memh->mr->rkey,
             memh->umr ? memh->umr->lkey : 0,
             memh->umr ? memh->umr->rkey : 0
             );
}

static ucs_rcache_ops_t uct_ib_rcache_ops = {
    .mem_reg     = uct_ib_rcache_mem_reg_cb,
    .mem_dereg   = uct_ib_rcache_mem_dereg_cb,
    .dump_region = uct_ib_rcache_dump_region_cb
};

static void uct_ib_make_md_name(char md_name[UCT_MD_NAME_MAX], struct ibv_device *device)
{
    snprintf(md_name, UCT_MD_NAME_MAX, "%s/%s", UCT_IB_MD_PREFIX, device->name);
}

static ucs_status_t uct_ib_query_md_resources(uct_md_resource_desc_t **resources_p,
                                              unsigned *num_resources_p)
{
    uct_md_resource_desc_t *resources;
    struct ibv_device **device_list;
    ucs_status_t status;
    int i, num_devices;

    /* Get device list from driver */
    device_list = ibv_get_device_list(&num_devices);
    if (device_list == NULL) {
        ucs_debug("Failed to get IB device list, assuming no devices are present");
        status = UCS_ERR_NO_DEVICE;
        goto out;
    }

    resources = ucs_calloc(num_devices, sizeof(*resources), "ib resources");
    if (resources == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto out_free_device_list;
    }

    for (i = 0; i < num_devices; ++i) {
        uct_ib_make_md_name(resources[i].md_name, device_list[i]);
    }

    *resources_p     = resources;
    *num_resources_p = num_devices;
    status = UCS_OK;

out_free_device_list:
    ibv_free_device_list(device_list);
out:
    return status;
}

static void uct_ib_fork_warn()
{
    ucs_warn("ibv_fork_init() was not successful, yet a fork() has been issued.");
}

static void uct_ib_fork_warn_enable()
{
    static volatile uint32_t enabled = 0;
    int ret;

    if (ucs_atomic_cswap32(&enabled, 0, 1) != 0) {
        return;
    }

    ret = pthread_atfork(uct_ib_fork_warn, NULL, NULL);
    if (ret) {
        ucs_warn("ibv_fork_init failed, and registering atfork warning failed too: %m");
    }
}

static ucs_status_t
uct_ib_md_open(const char *md_name, const uct_md_config_t *uct_md_config, uct_md_h *md_p)
{
    const uct_ib_md_config_t *md_config = ucs_derived_of(uct_md_config, uct_ib_md_config_t);
    struct ibv_device **ib_device_list, *ib_device;
    char tmp_md_name[UCT_MD_NAME_MAX];
    ucs_rcache_params_t rcache_params;
    ucs_status_t status;
    int i, num_devices, ret;
    uct_ib_md_t *md;

    /* Get device list from driver */
    ib_device_list = ibv_get_device_list(&num_devices);
    if (ib_device_list == NULL) {
        ucs_debug("Failed to get IB device list, assuming no devices are present");
        status = UCS_ERR_NO_DEVICE;
        goto out;
    }

    ib_device = NULL;
    for (i = 0; i < num_devices; ++i) {
        uct_ib_make_md_name(tmp_md_name, ib_device_list[i]);
        if (!strcmp(tmp_md_name, md_name)) {
            ib_device = ib_device_list[i];
            break;
        }
    }
    if (ib_device == NULL) {
        status = UCS_ERR_NO_DEVICE;
        goto out_free_dev_list;
    }

    md = ucs_malloc(sizeof(*md), "ib_md");
    if (md == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto out_free_dev_list;
    }

    md->super.ops       = &uct_ib_md_ops;
    md->super.component = &uct_ib_mdc;

    /* Create statistics */
    status = UCS_STATS_NODE_ALLOC(&md->stats, &uct_ib_md_stats_class, NULL,
                                  "%s-%p", ibv_get_device_name(ib_device), md);
    if (status != UCS_OK) {
        goto err_free_md;
    }

    if (md_config->fork_init != UCS_NO) {
        ret = ibv_fork_init();
        if (ret) {
            if (md_config->fork_init == UCS_YES) {
                ucs_error("ibv_fork_init() failed: %m");
                status = UCS_ERR_IO_ERROR;
                goto err_release_stats;
            }
            ucs_debug("ibv_fork_init() failed: %m, continuing, but fork may be unsafe.");
            uct_ib_fork_warn_enable();
        }
    }

    status = uct_ib_device_init(&md->dev, ib_device UCS_STATS_ARG(md->stats));
    if (status != UCS_OK) {
        goto err_release_stats;
    }

    /* Allocate memory domain */
    md->pd = ibv_alloc_pd(md->dev.ibv_context);
    if (md->pd == NULL) {
        ucs_error("ibv_alloc_pd() failed: %m");
        status = UCS_ERR_NO_MEMORY;
        goto err_cleanup_device;
    }

    md->eth_pause   = md_config->eth_pause;
    md->prefetch_mr = md_config->prefetch_mr;
    md->rcache      = NULL;
    md->reg_cost    = md_config->uc_reg_cost;

    if (md_config->rcache.enable != UCS_NO) {
        rcache_params.region_struct_size = sizeof(uct_ib_rcache_region_t);
        rcache_params.ucm_event_priority = md_config->rcache.event_prio;
        rcache_params.context            = md;
        rcache_params.ops                = &uct_ib_rcache_ops;
        status = ucs_rcache_create(&rcache_params, uct_ib_device_name(&md->dev)
                                   UCS_STATS_ARG(md->stats), &md->rcache);
        if (status == UCS_OK) {
            md->super.ops         = &uct_ib_md_rcache_ops;
            md->reg_cost.overhead = md_config->rcache.overhead;
            md->reg_cost.growth   = 0; /* It's close enough to 0 */
        } else {
            ucs_assert(md->rcache == NULL);
            if (md_config->rcache.enable == UCS_YES) {
                ucs_error("Failed to create registration cache: %s",
                          ucs_status_string(status));
                goto err_dealloc_pd;
            } else {
                ucs_debug("Could not create registration cache for: %s",
                          ucs_status_string(status));
            }
        }
    }

    if (md_config->odp_max_size == UCS_CONFIG_MEMUNITS_AUTO) {
        md->odp_max_size = uct_ib_device_odp_max_size(&md->dev);
    } else {
        md->odp_max_size = md_config->odp_max_size;
    }

    if (uct_ib_md_umr_qp_create(md) != UCS_OK) {
#if HAVE_EXP_UMR
        md->umr_qp = NULL;
        md->umr_cq = NULL;
#endif
    }

    *md_p = &md->super;
    status = UCS_OK;

out_free_dev_list:
    ibv_free_device_list(ib_device_list);
out:
    return status;

err_dealloc_pd:
    ibv_dealloc_pd(md->pd);
err_cleanup_device:
    uct_ib_device_cleanup(&md->dev);
err_release_stats:
    UCS_STATS_NODE_FREE(md->stats);
err_free_md:
    ucs_free(md);
    goto out_free_dev_list;
}

static void uct_ib_md_close(uct_md_h uct_md)
{
    uct_ib_md_t *md = ucs_derived_of(uct_md, uct_ib_md_t);

    if (md->rcache != NULL) {
        ucs_rcache_destroy(md->rcache);
    }
    uct_ib_md_umr_qp_destroy(md);
    ibv_dealloc_pd(md->pd);
    uct_ib_device_cleanup(&md->dev);
    UCS_STATS_NODE_FREE(md->stats);
    ucs_free(md);
}

UCT_MD_COMPONENT_DEFINE(uct_ib_mdc, UCT_IB_MD_PREFIX,
                        uct_ib_query_md_resources, uct_ib_md_open, NULL,
                        uct_ib_rkey_unpack,
                        (void*)ucs_empty_function_return_success /* release */,
                        "IB_", uct_ib_md_config_table, uct_ib_md_config_t);
