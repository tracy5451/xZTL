/* xZTL: Zone Translation Layer User-space Library
 *
 * Copyright 2019 Samsung Electronics
 *
 * Written by Ivan L. Picoli <i.picoli@samsung.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#include <stdlib.h>
#include <sys/queue.h>
#include <xztl-mempool.h>
#include <xztl-ztl.h>
#include <xztl.h>
#include <ztl.h>
#include <ztl_metadata.h>

extern uint16_t    app_ngrps;
struct app_group   **glist;
static uint16_t    cur_grp[ZTL_PRO_TYPES];

void ztl_pro_free(struct app_pro_addr *ctx) {
    uint32_t zn_i;

    for (zn_i = 0; zn_i < ctx->naddr; zn_i++)
        ztl_pro_grp_free(ctx->grp, ctx->addr[zn_i].g.zone, ctx->nsec[zn_i]);

    /* We assume a single group for now */
    app_grp_ctx_sub(ctx->grp);
}

struct app_pro_addr *ztl_pro_new(uint32_t nsec, int32_t *node_id) {
    struct xztl_mp_entry *mpe;
    struct app_pro_addr * ctx;
    struct app_group *    grp;
    int                   ret;

    ZDEBUG(ZDEBUG_PRO, "ztl-pro  (new): nsec %d, node_id %d", nsec, *node_id);

    mpe = xztl_mempool_get(XZTL_ZTL_PRO_CTX, 0);
    if (!mpe) {
        log_erra("ztl-pro: mempool is empty. node_id %d", *node_id);
        return NULL;
    }

    ctx = (struct app_pro_addr *)mpe->opaque;

    /* For now, we consider a single group */
    grp = glist[0];

    ret = ztl_pro_grp_get(grp, ctx, nsec, node_id, NULL);
    if (ret) {
        log_erra("ztl-pro: Get group zone failed. node_id %d", *node_id);
        xztl_mempool_put(mpe, XZTL_ZTL_PRO_CTX, 0);
        return NULL;
    }

    ctx->grp = grp;
    app_grp_ctx_add(grp);

    return ctx;
}

void ztl_pro_check_gc(struct app_group *grp) {
}

void ztl_pro_exit(void) {
    int ret;

    ret = ztl()->groups.get_list_fn(glist, app_ngrps);
    if (ret != app_ngrps)
        log_infoa("ztl-pro (exit): Groups mismatch (%d,%d).", ret, app_ngrps);

    while (ret) {
        ret--;
        ztl_pro_grp_exit(glist[ret]);
    }
	xztl_mempool_destroy(XZTL_NODE_MGMT_ENTRY, 0);
	xztl_mempool_destroy(XZTL_ZTL_PRO_CTX, 0);

    free(glist);
    log_info("ztl-pro: Global provisioning stopped.");
}

int ztl_pro_init(void) {
    int ret, grp_i = 0;

    glist = calloc(app_ngrps, sizeof(struct app_group *));
    if (!glist)
        return XZTL_ZTL_GROUP_ERR;

	ret = xztl_mempool_create(XZTL_ZTL_PRO_CTX, 0, ZTL_PRO_MP_SZ,
                                  sizeof(struct app_pro_addr), NULL, NULL);
    if (ret)
        goto FREE;

	ret = xztl_mempool_create(XZTL_NODE_MGMT_ENTRY, 0, 128,
                        sizeof(struct xnvme_node_mgmt_entry), NULL, NULL);
    if (ret)
        goto MP;

    ret = ztl()->groups.get_list_fn(glist, app_ngrps);
    if (ret != app_ngrps)
        goto MP1;
    if (ztl_metadata_init(glist[0]))
        goto EXIT;
    for (grp_i = 0; grp_i < app_ngrps; grp_i++) {
        if (ztl_pro_grp_node_init(glist[grp_i]))
            goto EXIT;
    }

    memset(cur_grp, 0x0, sizeof(uint16_t) * ZTL_PRO_TYPES);
    log_info("ztl-pro: Global provisioning started.");

    return XZTL_OK;

EXIT:
    while (grp_i) {
        grp_i--;
        ztl_pro_grp_exit(glist[grp_i]);
    }
MP1:
    xztl_mempool_destroy(XZTL_NODE_MGMT_ENTRY, 0);
MP:
    xztl_mempool_destroy(XZTL_ZTL_PRO_CTX, 0);
FREE:
    free(glist);
    return XZTL_ZTL_GROUP_ERR;
}

static struct app_pro_mod ztl_pro = {.mod_id         = LIBZTL_PRO,
                                     .name           = "LIBZTL-PRO",
                                     .init_fn        = ztl_pro_init,
                                     .exit_fn        = ztl_pro_exit,
                                     .check_gc_fn    = ztl_pro_check_gc,
                                     .new_fn         = ztl_pro_new,
                                     .free_fn        = ztl_pro_free,
                                     .submit_node_fn = ztl_pro_grp_submit_mgmt};

void ztl_pro_register(void) {
    ztl_mod_register(ZTLMOD_PRO, LIBZTL_PRO, &ztl_pro);
}
