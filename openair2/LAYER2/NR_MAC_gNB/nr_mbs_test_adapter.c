/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* TEMPORARY test adapter for MBS PTM scheduling, see nr_mbs_test_adapter.h */

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <unistd.h>
#include "nr_mbs_test_adapter.h"
#include "mac_proto.h"
#include "nr_radio_config.h"
#include "common/config/config_paramdesc.h"
#include "common/config/config_userapi.h"
#include "common/utils/system.h"
#include "executables/softmodem-common.h"
#include "openair2/LAYER2/nr_pdcp/nr_pdcp_mbs_test.h"
#include "openair2/LAYER2/nr_pdcp/nr_pdcp_oai_api.h"
#include "openair2/SDAP/nr_sdap/nr_sdap.h"

/* time between two attempts to make the UEs of the cell join the group, and between two statistics outputs */
#define NR_MBS_TEST_JOIN_PERIOD_MS 100
#define NR_MBS_TEST_STATS_PERIOD_MS 1000

typedef struct {
  gNB_MAC_INST *mac;
  nr_cell_sched_t *cell;
  nr_mbs_group_config_t group_config;
  /// PDCP context of the MBS session, set once the group exists
  ue_id_t pdcp_context;
  int sdu_size;
  int sdu_interval_ms;
  ue_id_t shared_delivery_ue_id;
  int shared_delivery_pdu_session;
  /// create a group that every UE joins, or only feed the groups of the UEs that joined through the 5GC
  bool join_ues;
  /// with join_ues = 0, the destination of the test datagrams
  struct in_addr udp_dst_addr;
  uint16_t udp_dst_port;
} nr_mbs_test_adapter_t;

static nr_mbs_test_adapter_t *adapter;

/* a UE joins a multicast session through its PDU session (TS 23.247 §7.2.1.3), so only UEs with a DRB join */
static bool ue_can_join(NR_UE_info_t *UE, const nr_cell_sched_t *cell)
{
  bool has_drb = false;
  FOR_EACH_SEQ_ARR(const nr_lc_config_t *, c, &UE->UE_sched_ctrl.lc_config)
    has_drb |= c->lcid >= DL_SCH_LCID_DTCH;
  return UE->pcell == cell && nr_mac_ue_is_active(UE) && UE->current_DL_BWP.bwp_id == 0
         && UE->UE_sched_ctrl.available_dl_harq.len > 0 && has_drb;
}

static void join_ues(nr_mbs_test_adapter_t *a, nr_mbs_group_t *group)
{
  FOR_EACH_CONNECTED_UE(&a->mac->UE_info, UE) {
    if (ue_can_join(UE, a->cell))
      nr_mac_add_mbs_member(a->mac, group, UE);
  }
}

/* the DL traffic of one PDU session stands in for the traffic of the shared N3 tunnel of the MBS session */
static bool shared_delivery(ue_id_t ue_id, int pdusession_id, const uint8_t *buf, int size)
{
  const nr_mbs_test_adapter_t *a = adapter;
  if (!a->pdcp_context || ue_id != a->shared_delivery_ue_id || pdusession_id != a->shared_delivery_pdu_session)
    return false;
  nr_pdcp_mrb_data_req(a->pdcp_context, a->group_config.mrb_id, buf, size);
  return true;
}

/* join_ues = 0: the numbered test SDUs go to every multicast group with members, standing in for its shared N3 tunnel */
static void *nr_mbs_test_feed_thread(void *arg)
{
  nr_mbs_test_adapter_t *a = arg;
  gNB_MAC_INST *mac = a->mac;
  /* an IPv4/UDP datagram, so that it reaches an application on the UE; the UDP checksum is optional in IPv4. The
   * source is 192.0.2.1 (TEST-NET-1, RFC 5737): a loopback source would be dropped by the receiving kernel. */
  uint8_t sdu[a->sdu_size];
  for (int i = 0; i < a->sdu_size; i++)
    sdu[i] = i & 0xff;
  struct iphdr *ip = (struct iphdr *)sdu;
  *ip = (struct iphdr){.version = 4, .ihl = 5, .tot_len = htons(a->sdu_size), .ttl = 64, .protocol = IPPROTO_UDP,
                       .saddr = htonl(0xc0000201), .daddr = a->udp_dst_addr.s_addr};
  uint32_t sum = 0;
  for (int i = 0; i < 10; i++)
    sum += ((uint16_t *)ip)[i];
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  ip->check = ~sum;
  struct udphdr *udp = (struct udphdr *)(ip + 1);
  *udp = (struct udphdr){.source = htons(a->udp_dst_port), .dest = htons(a->udp_dst_port),
                         .len = htons(a->sdu_size - sizeof(*ip))};
  uint8_t *payload = (uint8_t *)(udp + 1);
  uint32_t seq = 0;
  int t_ms = 0;
  while (!oai_exit) {
    rnti_t g_rnti[NR_MAX_MBS_GROUPS];
    int mrb_id[NR_MAX_MBS_GROUPS];
    int n = 0;
    NR_SCHED_LOCK(&mac->sched_lock);
    for (nr_mbs_group_t *const *g = mac->UE_info.mbs_group_list; *g != NULL; g++) {
      if ((*g)->config.broadcast || (*g)->num_members == 0)
        continue;
      if (t_ms % NR_MBS_TEST_STATS_PERIOD_MS == 0)
        LOG_I(NR_MAC,
              "MBS test: G-RNTI %04x members %d SDUs injected %u PTM transmissions %lu SDU bytes %lu ACK %lu NACK %lu\n",
              (*g)->g_rnti, (*g)->num_members, seq, (*g)->ptm_tx, (*g)->ptm_sdu_bytes, (*g)->harq_ack, (*g)->harq_nack);
      g_rnti[n] = (*g)->g_rnti;
      mrb_id[n++] = (*g)->config.mrb_id;
    }
    NR_SCHED_UNLOCK(&mac->sched_lock);
    if (n > 0) {
      /* the first 4 bytes of the payload number the datagrams, so that a receiver can count losses */
      uint32_t seq_be = htonl(seq++);
      memcpy(payload, &seq_be, sizeof(seq_be));
      for (int i = 0; i < n; i++)
        nr_pdcp_mrb_data_req(NR_PDCP_MBS_CONTEXT_ID(g_rnti[i]), mrb_id[i], sdu, a->sdu_size);
    }
    usleep(a->sdu_interval_ms * 1000);
    t_ms += a->sdu_interval_ms;
  }
  return NULL;
}

static void *nr_mbs_test_thread(void *arg)
{
  nr_mbs_test_adapter_t *a = arg;
  gNB_MAC_INST *mac = a->mac;

  NR_SCHED_LOCK(&mac->sched_lock);
  nr_mbs_group_t *group = nr_mac_add_mbs_group(mac, a->cell, &a->group_config);
  NR_SCHED_UNLOCK(&mac->sched_lock);
  AssertFatal(group, "MBS test adapter: cannot create the MBS group\n");
  const rnti_t g_rnti = group->g_rnti;
  const ue_id_t pdcp_context = NR_PDCP_MBS_CONTEXT_ID(g_rnti);

  uint8_t sdu[a->sdu_size];
  uint32_t seq = 0;
  for (int i = 0; i < a->sdu_size; i++)
    sdu[i] = i & 0xff;
  const int period_ms = a->sdu_interval_ms > 0 ? a->sdu_interval_ms : NR_MBS_TEST_JOIN_PERIOD_MS;
  int t_ms = 0;
  while (!oai_exit) {
    if (t_ms % NR_MBS_TEST_JOIN_PERIOD_MS == 0) {
      NR_SCHED_LOCK(&mac->sched_lock);
      join_ues(a, group);
      const bool active = group->num_members > 0;
      NR_SCHED_UNLOCK(&mac->sched_lock);
      /* stands in for the setup of the MRB in the gNB-CU-UP when the session gets its first member */
      if (active && !a->pdcp_context) {
        nr_pdcp_add_mrb(true, pdcp_context, a->group_config.mrb_id, 0, NR_MBS_TEST_PDCP_SN_SIZE, NR_MBS_TEST_PDCP_T_REORDERING);
        a->pdcp_context = pdcp_context;
      }
    }
    if (t_ms % NR_MBS_TEST_STATS_PERIOD_MS == 0) {
      NR_SCHED_LOCK(&mac->sched_lock);
      LOG_I(NR_MAC,
            "MBS test: G-RNTI %04x members %d SDUs injected %u PTM transmissions %lu SDU bytes %lu pending %u ACK %lu NACK %lu\n",
            g_rnti,
            group->num_members,
            seq,
            group->ptm_tx,
            group->ptm_sdu_bytes,
            group->pending_bytes,
            group->harq_ack,
            group->harq_nack);
      NR_SCHED_UNLOCK(&mac->sched_lock);
    }
    if (a->sdu_interval_ms > 0 && a->pdcp_context) {
      /* the first 4 bytes number the SDUs, so that a receiver can count losses */
      uint32_t seq_be = htonl(seq++);
      memcpy(sdu, &seq_be, sizeof(seq_be));
      nr_pdcp_mrb_data_req(pdcp_context, a->group_config.mrb_id, sdu, a->sdu_size);
    }
    usleep(period_ms * 1000);
    t_ms += period_ms;
  }
  return NULL;
}

void nr_mbs_test_adapter_init(gNB_MAC_INST *mac, nr_cell_sched_t *cell)
{
  GET_PARAMS_LIST(mbs_list, mbs_params, MBS_TEST_PARAMS_DESC, CONFIG_STRING_MBS_TEST, NULL);
  if (mbs_list.numelt == 0)
    return;
  const paramdef_t *p = mbs_list.paramarray[0];
  const int np = sizeofArray(mbs_params);
  nr_mbs_test_adapter_t *a = calloc_or_fail(1, sizeof(*a));
  a->mac = mac;
  a->cell = cell;
  const int dci_format = *gpd(p, np, CONFIG_STRING_MBS_TEST_DCI_FORMAT)->iptr;
  if (dci_format != 41 && dci_format != 42) {
    LOG_E(NR_MAC, "MBS DCI format must be 41 or 42\n");
    free(a);
    return;
  }
  a->group_config = (nr_mbs_group_config_t){
      .dci_4_2 = dci_format == 42,
      .config_id = 0,
      .mrb_id = *gpd(p, np, CONFIG_STRING_MBS_TEST_MRB_ID)->iptr,
      .lcid = *gpd(p, np, CONFIG_STRING_MBS_TEST_LCID)->iptr,
      .harq_feedback = *gpd(p, np, CONFIG_STRING_MBS_TEST_HARQ_FEEDBACK)->iptr ? NR_MBS_HARQ_FEEDBACK_ACK_NACK
                                                                                : NR_MBS_HARQ_FEEDBACK_DISABLED,
      .num_harq = *gpd(p, np, CONFIG_STRING_MBS_TEST_NUM_HARQ)->iptr,
  };
  a->sdu_size = *gpd(p, np, CONFIG_STRING_MBS_TEST_SDU_SIZE)->iptr;
  a->sdu_interval_ms = *gpd(p, np, CONFIG_STRING_MBS_TEST_SDU_INTERVAL)->iptr;
  a->shared_delivery_ue_id = *gpd(p, np, CONFIG_STRING_MBS_TEST_SHARED_DELIVERY_UE)->iptr;
  a->shared_delivery_pdu_session = *gpd(p, np, CONFIG_STRING_MBS_TEST_SHARED_DELIVERY_PDU_SESSION)->iptr;
  a->join_ues = *gpd(p, np, CONFIG_STRING_MBS_TEST_JOIN_UES)->iptr != 0;
  AssertFatal(a->sdu_size >= sizeof(uint32_t), "MBS test: invalid SDU size\n");
  if (!a->join_ues) {
    AssertFatal(a->sdu_interval_ms > 0, "MBS test: join_ues = 0 needs sdu_interval_ms\n");
    AssertFatal(a->sdu_size >= sizeof(struct iphdr) + sizeof(struct udphdr) + sizeof(uint32_t), "MBS test: SDU too small\n");
    const char *dst = *gpd(p, np, CONFIG_STRING_MBS_TEST_UDP_DST_ADDR)->strptr;
    AssertFatal(inet_pton(AF_INET, dst, &a->udp_dst_addr) == 1, "MBS test: invalid udp_dst_addr %s\n", dst);
    a->udp_dst_port = *gpd(p, np, CONFIG_STRING_MBS_TEST_UDP_DST_PORT)->iptr;
    LOG_W(NR_MAC, "MBS test adapter enabled: UDP datagrams of %d bytes to %s:%d every %d ms on every multicast group with members\n",
          a->sdu_size, dst, a->udp_dst_port, a->sdu_interval_ms);
    pthread_t t;
    threadCreate(&t, nr_mbs_test_feed_thread, a, "MBS_TEST", -1, OAI_PRIORITY_RT_LOW);
    return;
  }
  adapter = a;
  if (a->shared_delivery_ue_id > 0)
    nr_sdap_set_dl_redirect(shared_delivery);
  LOG_W(NR_MAC,
        "MBS test adapter enabled: MRB %d LCID %d, test SDUs every %d ms, DL of UE %lu PDU session %d delivered on the MRB\n",
        a->group_config.mrb_id,
        a->group_config.lcid,
        a->sdu_interval_ms,
        a->shared_delivery_ue_id,
        a->shared_delivery_pdu_session);
  pthread_t t;
  threadCreate(&t, nr_mbs_test_thread, a, "MBS_TEST", -1, OAI_PRIORITY_RT_LOW);
}
