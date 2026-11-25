/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "MbsContainers.h"
#include <ctype.h>
#include <string.h>
#include "common/utils/LOG/log.h"

/*
 * Requested MBS container, TS 24.501 clause 9.11.4.30 figure 9.11.4.30.2. One entry is:
 *
 *   octet 4   bits 1-2 Type of multicast MBS session ID, bits 3-4 MBS operation, bits 5-8 spare
 *   octet 5.. the session ID: 6 octets for a TMGI
 *
 * Entries are not length-prefixed; the length of each follows from its ID type.
 */
int encode_requested_mbs_container(uint8_t *buffer, uint32_t size, const requested_mbs_info_t *info, int num)
{
  uint32_t encoded = 0;

  if ((uint32_t)num * (1 + MBS_TMGI_LEN) > size) {
    LOG_E(NAS, "Requested MBS container: %d entries do not fit in %u octets\n", num, size);
    return -1;
  }

  for (int i = 0; i < num; i++) {
    buffer[encoded++] = (uint8_t)(((info[i].operation & 0x03) << 2) | (MBS_SESSION_ID_TMGI & 0x03));
    memcpy(buffer + encoded, info[i].tmgi, MBS_TMGI_LEN);
    encoded += MBS_TMGI_LEN;
  }

  return encoded;
}

/*
 * Received MBS container, TS 24.501 clause 9.11.4.31 figure 9.11.4.31.2. One entry is:
 *
 *   octet 4   bits 1-3 MBS decision (MD), bits 4-5 MBS service area indication (MSAI),
 *             bits 6-8 Rejection cause
 *   octet 5   bit 1 IPAE, bits 2-3 MTI, bit 4 MSCI, bit 5 IPAT, bits 6-8 spare
 *   octet 6.. TMGI (6 octets), then whatever the flags of octet 5 switched on
 *
 * The optional parts are skipped rather than kept: the UE has no use for them until it has a
 * multicast MRB. They still have to be walked over, or the next entry cannot be found -- which is
 * why an entry whose flags the decoder cannot size ends the decode.
 */
int decode_received_mbs_container(received_mbs_container_t *out, const uint8_t *buffer, uint32_t length)
{
  uint32_t pos = 0;

  if (!out || !buffer)
    return -1;

  memset(out, 0, sizeof(*out));

  while (pos < length) {
    if (pos + 2 + MBS_TMGI_LEN > length) {
      LOG_E(NAS, "Received MBS container: entry at octet %u runs past the IE (%u octets)\n", pos, length);
      return -1;
    }

    uint8_t octet4 = buffer[pos];
    uint8_t octet5 = buffer[pos + 1];
    uint8_t decision = octet4 & 0x07;
    uint8_t msai = (octet4 >> 3) & 0x03;
    uint8_t cause = (octet4 >> 5) & 0x07;
    bool ipae = octet5 & 0x01;
    uint8_t mti = (octet5 >> 1) & 0x03;
    bool msci = (octet5 >> 3) & 0x01;
    bool ipv6 = (octet5 >> 4) & 0x01;

    pos += 2;

    if (out->num < MBS_MAX_SESSIONS) {
      received_mbs_info_t *info = &out->info[out->num];
      info->decision = (mbs_decision_t)decision;
      info->reject_cause = (mbs_reject_cause_t)cause;
      memcpy(info->tmgi, buffer + pos, MBS_TMGI_LEN);
      out->num++;
    } else {
      LOG_W(NAS, "Received MBS container: more than %d entries, the rest is ignored\n", MBS_MAX_SESSIONS);
    }
    pos += MBS_TMGI_LEN;

    /* "If IPAE is set to 'Source and destination IP address information included', Source IP address
     * information and Destination IP address information shall be included" -- a pair of addresses,
     * v4 or v6 as IPAT says. */
    if (ipae)
      pos += ipv6 ? 32 : 8;

    /* The MBS service area is a 5GS tracking area identity list, an NR CGI list, or both, and only
     * the NR CGI list carries its own length. A TAI list cannot be sized without decoding it, so a
     * container that has one ends the decode here rather than guessing. */
    if (msai == 1 || msai == 3) {
      LOG_W(NAS, "Received MBS container: an MBS TAI list is not decoded, stopping after %d entrie(s)\n", out->num);
      return pos;
    }
    if (msai == 2) {
      if (pos >= length) {
        LOG_E(NAS, "Received MBS container: NR CGI list runs past the IE\n");
        return -1;
      }
      pos += 1 + buffer[pos]; /* Length of NR CGI list contents, then the list */
    }

    /* MBS timers: 6 octets of MBS start time, or one octet of back-off timer */
    if (mti == 1)
      pos += 6;
    else if (mti == 2)
      pos += 1;

    /* MBS security container: a count, then 43 octets per MBS security keys set (figure
     * 9.11.4.31.11: MTKI flag, Key domain ID, MSK ID, MSK, MTK ID, Encrypted MTK). */
    if (msci) {
      if (pos >= length) {
        LOG_E(NAS, "Received MBS container: security container runs past the IE\n");
        return -1;
      }
      pos += 1 + (uint32_t)buffer[pos] * 43;
    }

    if (pos > length) {
      LOG_E(NAS, "Received MBS container: an entry's optional fields run past the IE\n");
      return -1;
    }
  }

  return pos;
}

const char *mbs_decision_text(mbs_decision_t decision)
{
  switch (decision) {
    case MBS_DECISION_SERVICE_AREA_UPDATE: return "MBS service area update";
    case MBS_DECISION_JOIN_ACCEPTED: return "MBS join is accepted";
    case MBS_DECISION_JOIN_REJECTED: return "MBS join is rejected";
    case MBS_DECISION_REMOVE_UE: return "Remove UE from multicast MBS session";
    case MBS_DECISION_SECURITY_UPDATE: return "MBS security information update";
    default: return "unknown";
  }
}

const char *mbs_reject_cause_text(mbs_reject_cause_t cause)
{
  switch (cause) {
    case MBS_REJECT_NO_INFO: return "no additional information provided";
    case MBS_REJECT_INSUFFICIENT_RESOURCES: return "insufficient resources";
    case MBS_REJECT_NOT_AUTHORIZED: return "user is not authorized to use MBS service";
    case MBS_REJECT_NOT_STARTED: return "multicast MBS session has not started or will not start soon";
    case MBS_REJECT_OUTSIDE_SERVICE_AREA: return "user is outside of local MBS service area";
    case MBS_REJECT_CONTEXT_NOT_FOUND: return "session context not found";
    case MBS_REJECT_SESSION_RELEASED: return "multicast MBS session is released";
    default: return "unknown";
  }
}

bool mbs_tmgi_from_string(const char *in, uint8_t *out)
{
  if (!in || !out)
    return false;
  if (strlen(in) != 2 * MBS_TMGI_LEN)
    return false;

  for (int i = 0; i < MBS_TMGI_LEN; i++) {
    char hi = in[2 * i], lo = in[2 * i + 1];
    if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo))
      return false;
    out[i] = (uint8_t)((isdigit((unsigned char)hi) ? hi - '0' : (tolower((unsigned char)hi) - 'a' + 10)) << 4);
    out[i] |= (uint8_t)(isdigit((unsigned char)lo) ? lo - '0' : (tolower((unsigned char)lo) - 'a' + 10));
  }

  return true;
}
