/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "PduSessionModificationRequest.h"
#include <string.h>
#include "common/utils/LOG/log.h"

/**
 * @brief PDU session modification request (8.3.7 of 3GPP TS 24.501)
 *        UE to network
 *
 * The Requested MBS container is a type 6 information element (clause 9.11.4.30), i.e. TLV-E: one
 * octet of IEI, two of length, then the contents.
 */
int encode_pdu_session_modification_request(const pdu_session_modification_request_msg *msg, uint8_t *buffer, uint32_t len)
{
  uint32_t encoded = 0;

  if (len < 4) {
    LOG_E(NAS, "PDU Session Modification Request does not fit in %u octets\n", len);
    return -1;
  }

  buffer[encoded++] = msg->protocoldiscriminator;
  buffer[encoded++] = msg->pdusessionid;
  buffer[encoded++] = msg->pti;
  buffer[encoded++] = msg->pdusessionmodificationrequestmsgtype;

  if (msg->requested_mbs_present) {
    uint8_t contents[MBS_MAX_SESSIONS * (1 + MBS_TMGI_LEN)];
    int contents_len = encode_requested_mbs_container(contents, sizeof(contents), msg->requested_mbs, msg->num_requested_mbs);

    if (contents_len < 0)
      return -1;
    if (encoded + 3 + (uint32_t)contents_len > len) {
      LOG_E(NAS, "Requested MBS container does not fit in %u octets\n", len);
      return -1;
    }

    buffer[encoded++] = IEI_MODREQ_REQUESTED_MBS_CONTAINER;
    buffer[encoded++] = (uint8_t)((contents_len >> 8) & 0xff);
    buffer[encoded++] = (uint8_t)(contents_len & 0xff);
    memcpy(buffer + encoded, contents, contents_len);
    encoded += contents_len;
  }

  return encoded;
}
