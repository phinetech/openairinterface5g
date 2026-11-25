/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief PDU session modification request procedures
 */

#ifndef PDU_SESSION_MODIFICATION_REQUEST_H_
#define PDU_SESSION_MODIFICATION_REQUEST_H_

#include <stdint.h>
#include <stdbool.h>
#include "MessageType.h"
#include "MbsContainers.h"

/* PDU Session Modification Request optional IE identifiers, TS 24.501 table 8.3.7.1.1. Only the one
 * this build sends is listed; the rest are added as they are needed. */
#define IEI_MODREQ_REQUESTED_MBS_CONTAINER 0x70 /* Requested MBS container 9.11.4.30, TLV-E */

/*
 * Message name: PDU session modification request
 * Description: sent by the UE to request a modification of a PDU session.
 *              See table 8.3.7.1.1 of 3GPP TS 24.501.
 * Significance: dual
 * Direction: UE to network
 */
typedef struct pdu_session_modification_request_msg_tag {
  /* Mandatory fields */
  uint8_t protocoldiscriminator;
  uint8_t pdusessionid;
  uint8_t pti;
  MessageType pdusessionmodificationrequestmsgtype;
  /* Optional: Requested MBS container (join or leave one or more multicast MBS sessions) */
  bool requested_mbs_present;
  uint8_t num_requested_mbs;
  requested_mbs_info_t requested_mbs[MBS_MAX_SESSIONS];
} pdu_session_modification_request_msg;

/** @brief Encode a PDU Session Modification Request into \p buffer of \p len octets.
 *  Returns the number of octets written, or -1 on failure. */
int encode_pdu_session_modification_request(const pdu_session_modification_request_msg *msg, uint8_t *buffer, uint32_t len);

#endif /* PDU_SESSION_MODIFICATION_REQUEST_H_ */
