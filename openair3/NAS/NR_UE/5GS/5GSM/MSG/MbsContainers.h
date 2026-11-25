/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief The two MBS containers a UE exchanges with the SMF to join or leave a multicast MBS
 *        session: the Requested MBS container it sends (TS 24.501 clause 9.11.4.30) and the
 *        Received MBS container it gets back (clause 9.11.4.31).
 */

#ifndef MBS_CONTAINERS_H_
#define MBS_CONTAINERS_H_

#include <stdint.h>
#include <stdbool.h>

/* A TMGI is 6 octets: 3 of MBS Service ID then the PLMN, "coded as described in clause 10.5.6.13 in
 * 3GPP TS 24.008 starting from octet 2" (TS 24.501 clause 9.11.4.30 table 9.11.4.30.1). */
#define MBS_TMGI_LEN 6

/* How many multicast MBS sessions one container may carry, here. The IE itself allows more. */
#define MBS_MAX_SESSIONS 4

/* Type of multicast MBS session ID, TS 24.501 clause 9.11.4.30 (bits 1 to 2 of octet 4) */
typedef enum {
  MBS_SESSION_ID_TMGI = 1,
  MBS_SESSION_ID_SSM_IPV4 = 2,
  MBS_SESSION_ID_SSM_IPV6 = 3,
} mbs_session_id_type_t;

/* MBS operation, same clause (bits 3 to 4 of octet 4) */
typedef enum {
  MBS_OP_JOIN = 1,
  MBS_OP_LEAVE = 2,
} mbs_operation_t;

/* MBS decision, TS 24.501 clause 9.11.4.31 (bits 1 to 3 of octet 4) */
typedef enum {
  MBS_DECISION_SERVICE_AREA_UPDATE = 1,
  MBS_DECISION_JOIN_ACCEPTED = 2,
  MBS_DECISION_JOIN_REJECTED = 3,
  MBS_DECISION_REMOVE_UE = 4,
  MBS_DECISION_SECURITY_UPDATE = 5,
} mbs_decision_t;

/* Rejection cause, same clause (bits 6 to 8 of octet 4) */
typedef enum {
  MBS_REJECT_NO_INFO = 0,
  MBS_REJECT_INSUFFICIENT_RESOURCES = 1,
  MBS_REJECT_NOT_AUTHORIZED = 2,
  MBS_REJECT_NOT_STARTED = 3,
  MBS_REJECT_OUTSIDE_SERVICE_AREA = 4,
  MBS_REJECT_CONTEXT_NOT_FOUND = 5,
  MBS_REJECT_SESSION_RELEASED = 6,
} mbs_reject_cause_t;

/* One "multicast MBS session information" of a Requested MBS container. Only the TMGI form is
 * produced: it is what a service announcement gives and what the MB-SMF indexes a session by. */
typedef struct {
  mbs_operation_t operation;
  uint8_t tmgi[MBS_TMGI_LEN];
} requested_mbs_info_t;

/* One "Received MBS information" of a Received MBS container. Only the fields the UE acts on are
 * kept; the service area, the timers and the security container are skipped over. */
typedef struct {
  mbs_decision_t decision;
  mbs_reject_cause_t reject_cause;
  uint8_t tmgi[MBS_TMGI_LEN];
} received_mbs_info_t;

typedef struct {
  uint8_t num;
  received_mbs_info_t info[MBS_MAX_SESSIONS];
} received_mbs_container_t;

/** @brief Encode the contents of a Requested MBS container (without IEI and length) into \p buffer.
 *  Returns the number of octets written, or -1 if \p size is too small. */
int encode_requested_mbs_container(uint8_t *buffer, uint32_t size, const requested_mbs_info_t *info, int num);

/** @brief Decode the contents of a Received MBS container (without IEI and length).
 *  Returns the number of octets consumed, or -1 when the container is malformed. */
int decode_received_mbs_container(received_mbs_container_t *out, const uint8_t *buffer, uint32_t length);

/** @brief The human-readable name of a decision / rejection cause, for logs. */
const char *mbs_decision_text(mbs_decision_t decision);
const char *mbs_reject_cause_text(mbs_reject_cause_t cause);

/** @brief Parse 12 hex characters into a 6-octet TMGI. Returns false when \p in is not that. */
bool mbs_tmgi_from_string(const char *in, uint8_t *out);

#endif /* MBS_CONTAINERS_H_ */
