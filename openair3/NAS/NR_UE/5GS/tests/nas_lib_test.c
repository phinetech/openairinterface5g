/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <arpa/inet.h>
#include "common/utils/LOG/log.h"
#include "common/utils/ds/byte_array.h"
#include "RegistrationAccept.h"
#include "fgs_service_request.h"
#include "fgmm_service_accept.h"
#include "fgmm_service_reject.h"
#include "fgmm_registration_reject.h"
#include "fgmm_authentication_failure.h"
#include "FGSNASSecurityModeReject.h"
#include "fgmm_authentication_reject.h"
#include "nr_nas_msg.h"
#include "MbsContainers.h"
#include "PduSessionModificationRequest.h"
#include "PduSessionModificationCommand.h"

configmodule_interface_t *uniqCfg = NULL;

/**
 * @brief Equality check for NAS Service Request enc/dec
 */
/** @brief Test regression of NAS Registration Accept decoding
 *         This test verifies that the NAS Registration Accept message
 *         is correctly decoded. It decodes a message as it received from
 *         the OAI CN5G and compares the output with the expected Registration Accept structure.
 *         The purpose of this regression test is to ensure that changes
 *         to the NAS decoder do not break the existing decoding behavior with OAI CN5G */
static void test_regression_registration_accept(void)
{
  // Registration Accept message
  registration_accept_msg orig = {
      .guti = malloc_or_fail(sizeof(*orig.guti)),
      .result = 0x01,
      .nas_allowed_nssai[0].sst = 0x01,
      .nas_allowed_nssai[1].sst = 0x02,
      .nas_allowed_nssai[2].sst = 0x03,
      .num_allowed_slices = 3,
      .config_nssai[0].sst = 0x01,
      .config_nssai[1].sst = 0x02,
      .config_nssai[2].sst = 0x03,
      .num_configured_slices = 3,
  };

  orig.guti->guti.typeofidentity = FGS_MOBILE_IDENTITY_5G_GUTI;
  orig.guti->guti.amfpointer = 0x01;
  orig.guti->guti.amfregionid = 0x02;
  orig.guti->guti.amfsetid = 0x01;
  orig.guti->guti.mccdigit1 = 0x00;
  orig.guti->guti.mccdigit2 = 0x00;
  orig.guti->guti.mccdigit3 = 0x01;
  orig.guti->guti.mncdigit1 = 0x00;
  orig.guti->guti.mncdigit2 = 0x01;
  orig.guti->guti.mncdigit3 = 0x0F;
  orig.guti->guti.tmsi = 0xb0207806;
  orig.guti->guti.spare = 0b1111;
  orig.guti->guti.oddeven = 0;

  // Encoded Registration Accept message from OAI CN5G
  uint8_t cn5g_msg[] = {0x01, 0x01, // Registration Result
                        0x77, 0x00, 0x0B, 0xF2, 0x00, 0xF1, 0x10, // 5G-GUTI
                        0x02, 0x00, 0x41, 0xB0, 0x20, 0x78, 0x06, // 5G-GUTI
                        0x15, 0x06, 0x01, 0x01, 0x01, 0x02, 0x01, 0x03, // Allowed N-NSSAI
                        0x31, 0x06, 0x01, 0x01, 0x01, 0x02, 0x01, 0x03}; // Configured N-NSSAI

  // Decode NAS Registration Accept
  int encoded_length;
  registration_accept_msg decoded = {0};
  byte_array_t ba = {.buf = cn5g_msg, .len = sizeof(cn5g_msg)};
  encoded_length = decode_registration_accept(&decoded, ba);
  AssertFatal(encoded_length >= 0, "encode_fgs_service_request() failed\n");

  // Compare the raw encoded buffer with expected encoded data
  bool ret = eq_fgmm_registration_accept(&orig, &decoded);
  AssertFatal(ret, "Encoding mismatch!\n");

  // Free dynamic allocated memory
  free_fgmm_registration_accept(&orig);
  free_fgmm_registration_accept(&decoded);

  // The same message cut inside the 5G-GUTI IE (cn5g_msg[2] to [15]) is rejected
  for (size_t len = 3; len < 16; len++) {
    registration_accept_msg truncated = {0};
    byte_array_t ba_truncated = {.buf = cn5g_msg, .len = len};
    int ret = decode_registration_accept(&truncated, ba_truncated);
    AssertFatal(ret < 0, "decode_registration_accept() accepted the message cut to %zu octets\n", len);
    free_fgmm_registration_accept(&truncated);
  }

  // A 5G-GUTI IE carrying another type of identity (here SUCI) is rejected
  uint8_t not_guti[sizeof(cn5g_msg)];
  memcpy(not_guti, cn5g_msg, sizeof(not_guti));
  not_guti[5] = (not_guti[5] & ~0x07) | FGS_MOBILE_IDENTITY_SUCI;
  registration_accept_msg wrong_type = {0};
  int dec = decode_registration_accept(&wrong_type, (byte_array_t){.buf = not_guti, .len = sizeof(not_guti)});
  AssertFatal(dec < 0, "decode_registration_accept() accepted a 5G-GUTI IE with type of identity SUCI\n");
  free_fgmm_registration_accept(&wrong_type);
}

/**
 * @brief Test NAS Service Request enc/dec
 */
static void test_service_request(void)
{
  // Dummy NAS Service Request message
  uint16_t amf_set_id = 0x48;
  uint16_t amf_pointer = 0x34;
  uint32_t tmsi = 0x56789ABC;
  uint8_t container_data[] = {0xde, 0xad, 0xbe};
  fgs_service_request_msg_t original_msg = {
      .naskeysetidentifier = {.tsc = NAS_KEY_SET_IDENTIFIER_NATIVE, .naskeysetidentifier = NAS_KEY_SET_IDENTIFIER_NOT_AVAILABLE},
      .serviceType = SERVICE_TYPE_DATA,
      .fiveg_s_tmsi = {.digit1 = DIGIT1,
                       .typeofidentity = FGS_MOBILE_IDENTITY_5GS_TMSI,
                       .amfsetid = amf_set_id,
                       .amfpointer = amf_pointer,
                       .tmsi = tmsi},
      .has_uplink_data_status = true,
      .uplink_data_status = {[2] = PDU_SESSION_ACTIVE},
      .has_pdu_session_status = true,
      .pdu_session_status = {[2] = PDU_SESSION_ACTIVE, [4] = PDU_SESSION_ACTIVE},
      .fgsnasmessagecontainer = NULL};

  original_msg.fgsnasmessagecontainer = calloc_or_fail(1, sizeof(*original_msg.fgsnasmessagecontainer));
  FGCNasMessageContainer *nas_container = original_msg.fgsnasmessagecontainer;
  OctetString *nas_container_contents = &nas_container->nasmessagecontainercontents;
  nas_container_contents->value = calloc_or_fail(sizeof(container_data), sizeof(*nas_container_contents->value));
  memcpy(nas_container_contents->value, container_data, sizeof(container_data));
  nas_container_contents->length = sizeof(container_data);

  uint8_t expected_encoded_data[] = {0x17,
                                     0x00,
                                     0x07,
                                     0xF4,
                                     (amf_set_id >> 2) & 0xFF,
                                     (amf_set_id << 6) | (amf_pointer & 0x3F),
                                     (tmsi >> 24) & 0xFF,
                                     (tmsi >> 16) & 0xFF,
                                     (tmsi >> 8) & 0xFF,
                                     tmsi & 0xFF,
                                     IEI_UPLINK_DATA_STATUS,
                                     0x02,
                                     0x04,
                                     0x00,
                                     IEI_PDU_SESSION_STATUS,
                                     0x02,
                                     0x14,
                                     0x00,
                                     IEI_FGS_NAS_MESSAGE_CONTAINER,
                                     0x00,
                                     0x03,
                                     0xde,
                                     0xad,
                                     0xbe,
                                     IEI_UE_REQUEST_TYPE,
                                     0x01,
                                     0x00};
  int encoded_service_request_len = sizeof(expected_encoded_data) - 3;

  // Buffer
  uint8_t buffer[64];
  memset(buffer, 0, sizeof(buffer));

  // Encode NAS Service Request
  int encoded_length = encode_fgs_service_request(buffer, &original_msg, sizeof(buffer));
  AssertFatal(encoded_length >= 0, "encode_fgs_service_request() failed\n");
  AssertFatal(encoded_length == encoded_service_request_len, "Unexpected encoded SERVICE REQUEST length\n");

  // Compare the raw encoded buffer with expected encoded data, excluding the skipped optional IE.
  AssertFatal(memcmp(buffer, expected_encoded_data, encoded_length) == 0, "Encoding mismatch!\n");

  // Decode NAS Service Request
  fgs_service_request_msg_t decoded_service_request = {0};
  int decoded_length = decode_fgs_service_request(&decoded_service_request, expected_encoded_data, sizeof(expected_encoded_data));
  AssertFatal(decoded_length == sizeof(expected_encoded_data), "decode_fgs_service_request() failed\n");

  // Compare original and decoded messages
  AssertFatal(eq_fgs_service_request(&original_msg, &decoded_service_request),
              "test_service_request() failed: original and decoded messages do not match\n");
  free_fgs_service_request(&decoded_service_request);
  free_fgs_service_request(&original_msg);
}

/**
 * @brief Test NAS Service Accept enc/dec
 */
static void test_service_accept(void)
{
  // Dummy NAS Service Accept message
  fgs_service_accept_msg_t orig = {
      .has_psi_status = true,
      .has_psi_res = true,
      .num_errors = 1,
      .t3448 = malloc_or_fail(sizeof(*orig.t3448)),
  };
  orig.psi_status[1] = PDU_SESSION_ACTIVE;
  orig.psi_status[2] = PDU_SESSION_INACTIVE;
  orig.psi_status[3] = PDU_SESSION_ACTIVE;
  orig.psi_res[1] = REACTIVATION_FAILED;
  orig.psi_res[2] = REACTIVATION_SUCCESS;
  orig.cause[0].cause = Illegal_UE;
  orig.cause[0].pdu_session_id = 1;
  orig.t3448->value = 16;
  orig.t3448->unit = TWO_SECONDS;

  // Expected encoded data
  uint8_t expected_enc[] = {0x50, 0x02, 0x0a, 0x00, 0x26, 0x02, 0x02, 0x00, 0x72, 0x00, 0x00, 0x01, 0x03, 0x6B, 0x01, 0x10};
  uint16_t len_status = 2;
  expected_enc[9] = (len_status >> 8) & 0xFF;
  expected_enc[10] = len_status & 0xFF;

  // Buffer
  uint8_t buf[64] = {0};
  byte_array_t buffer = {.buf = &buf[0], .len = sizeof(expected_enc)};

  // Encode NAS Service Accept
  int encoded_length = encode_fgs_service_accept(&buffer, &orig);
  AssertFatal(encoded_length == sizeofArray(expected_enc), "encode_fgs_service_accept() failed\n");

  // Compare the raw encoded buffer with expected encoded data
  AssertFatal(memcmp(buffer.buf, expected_enc, buffer.len) == 0, "Encoding mismatch!\n");

  // Decode NAS Service Accept
  fgs_service_accept_msg_t dec = {0};
  int decoded_length = decode_fgs_service_accept(&dec, &buffer);
  AssertFatal(decoded_length >= 0, "decode_fgs_service_accept() failed\n");

  // Compare original and decoded messages
  AssertFatal(eq_service_accept(&orig, &dec), "test_service_accept() failed: original and decoded messages do not match\n");
  free_fgs_service_accept(&dec);
  free_fgs_service_accept(&orig);

  // The same message cut inside the PDU session reactivation result error cause IE (expected_enc[8] to [12])
  // or the T3448 value IE (expected_enc[13] to [15]) is rejected
  const size_t truncated_len[] = {9, 10, 11, 12, 14, 15};
  for (size_t i = 0; i < sizeofArray(truncated_len); i++) {
    byte_array_t truncated = {.buf = expected_enc, .len = truncated_len[i]};
    fgs_service_accept_msg_t dec_truncated = {0};
    AssertFatal(decode_fgs_service_accept(&dec_truncated, &truncated) < 0,
                "decode_fgs_service_accept() accepted the message cut to %zu octets\n",
                truncated_len[i]);
    free_fgs_service_accept(&dec_truncated);
  }

  // EAP message IE, which encode_fgs_service_accept() doesn't encode
  const uint8_t eap[] = {0x01, 0x02, 0x00, 0x04};
  uint8_t eap_enc[] = {IEI_EAPMSG, 0x00, 0x04, 0x01, 0x02, 0x00, 0x04};
  byte_array_t eap_ba = {.buf = eap_enc, .len = sizeof(eap_enc)};
  fgs_service_accept_msg_t dec_eap = {0};
  AssertFatal(decode_fgs_service_accept(&dec_eap, &eap_ba) == sizeof(eap_enc), "decode_fgs_service_accept() failed\n");
  AssertFatal(dec_eap.eap_msg.len == sizeof(eap) && memcmp(dec_eap.eap_msg.buf, eap, sizeof(eap)) == 0,
              "Service Accept: EAP message mismatch\n");
  free_fgs_service_accept(&dec_eap);
}

/** @brief Test NAS Service Reject enc/dec */
static void test_service_reject(void)
{
  // Dummy NAS Service Reject message
  fgs_service_reject_msg_t orig = {
      .cause = Illegal_UE,
      .has_psi_status = true,
      .t3446 = malloc_or_fail(sizeof(*orig.t3446)),
      .t3448 = malloc_or_fail(sizeof(*orig.t3448)),
  };
  orig.psi_status[1] = PDU_SESSION_INACTIVE; // PDU Session 1
  orig.psi_status[2] = PDU_SESSION_ACTIVE; // PDU Session 2
  orig.t3446->value = 10;
  orig.t3446->unit = TWO_SECONDS;
  orig.t3448->value = 1;
  orig.t3448->unit = ONE_MINUTE;

  // Expected encoded data
  uint8_t expected_enc[] = {0x03, 0x50, 0x02, 0x04, 0x00, 0x5F, 0x01, 0x0A, 0x6B, 0x01, 0x21};

  // Buffer
  uint8_t buf[64] = {0};
  byte_array_t buffer = {.buf = &buf[0], .len = sizeof(expected_enc)};

  // Encode NAS Service Reject
  int encoded_length = encode_fgs_service_reject(&buffer, &orig);
  AssertFatal(encoded_length == sizeofArray(expected_enc), "encode_fgs_service_reject() failed: %d != %ld\n", encoded_length, sizeofArray(expected_enc));

  // Compare the raw encoded buffer with expected encoded data
  AssertFatal(memcmp(buffer.buf, expected_enc, buffer.len) == 0, "Encoding mismatch!\n");

  // Decode NAS Service Reject
  fgs_service_reject_msg_t dec = {0};
  int decoded_length = decode_fgs_service_reject(&dec, &buffer);
  AssertFatal(decoded_length >= 0, "decode_fgs_service_reject() failed\n");

  // Compare original and decoded messages
  AssertFatal(eq_service_reject(&orig, &dec), "test_service_reject() failed: original and decoded messages do not match\n");
  free_fgs_service_reject(&dec);
  free_fgs_service_reject(&orig);

  // The same message cut inside the T3346 value IE (expected_enc[5] to [7]) or the T3448 value IE (expected_enc[8] to [10])
  // is rejected
  const size_t truncated_len[] = {6, 7, 9, 10};
  for (size_t i = 0; i < sizeofArray(truncated_len); i++) {
    byte_array_t truncated = {.buf = expected_enc, .len = truncated_len[i]};
    fgs_service_reject_msg_t dec_truncated = {0};
    AssertFatal(decode_fgs_service_reject(&dec_truncated, &truncated) < 0,
                "decode_fgs_service_reject() accepted the message cut to %zu octets\n",
                truncated_len[i]);
    free_fgs_service_reject(&dec_truncated);
  }

  // 5GMM cause, then a CAG information list IE, which encode_fgs_service_reject() doesn't encode: 16 octets announced, 1 present
  uint8_t cag_enc[] = {Illegal_UE, IEI_CAG_INFO_LIST, 0x00, 0x10, 0x01};
  byte_array_t cag_ba = {.buf = cag_enc, .len = sizeof(cag_enc)};
  fgs_service_reject_msg_t dec_cag = {0};
  AssertFatal(decode_fgs_service_reject(&dec_cag, &cag_ba) < 0, "decode_fgs_service_reject() accepted a truncated CAG list\n");
  free_fgs_service_reject(&dec_cag);

  // 5GMM cause, then an EAP message IE
  const uint8_t eap[] = {0x01, 0x02, 0x00, 0x04};
  uint8_t eap_enc[] = {Illegal_UE, IEI_EAPMSG, 0x00, 0x04, 0x01, 0x02, 0x00, 0x04};
  byte_array_t eap_ba = {.buf = eap_enc, .len = sizeof(eap_enc)};
  fgs_service_reject_msg_t dec_eap = {0};
  AssertFatal(decode_fgs_service_reject(&dec_eap, &eap_ba) == sizeof(eap_enc), "decode_fgs_service_reject() failed\n");
  AssertFatal(dec_eap.cause == Illegal_UE, "Service Reject: cause mismatch\n");
  AssertFatal(dec_eap.eap_msg && dec_eap.eap_msg->len == sizeof(eap) && memcmp(dec_eap.eap_msg->buf, eap, sizeof(eap)) == 0,
              "Service Reject: EAP message mismatch\n");
  free_fgs_service_reject(&dec_eap);
}

/** @brief Test NAS Registration Reject enc/dec (plain AMF reject: cause only) */
static void test_registration_reject(void)
{
  fgs_registration_reject_msg_t orig = {
      .cause = Illegal_UE,
  };

  uint8_t expected_enc[] = {0x03};

  uint8_t buf[64] = {0};
  byte_array_t buffer = {.buf = buf, .len = sizeof(expected_enc)};

  int encoded_length = encode_fgs_registration_reject(&buffer, &orig);
  AssertFatal(encoded_length == sizeofArray(expected_enc),
              "encode_fgs_registration_reject() failed: %d != %ld\n",
              encoded_length,
              sizeofArray(expected_enc));
  AssertFatal(memcmp(buffer.buf, expected_enc, buffer.len) == 0, "Encoding mismatch!\n");

  fgs_registration_reject_msg_t dec = {0};
  int decoded_length = decode_fgs_registration_reject(&dec, &buffer);
  AssertFatal(decoded_length >= 0, "decode_fgs_registration_reject() failed\n");
  AssertFatal(eq_registration_reject(&orig, &dec),
              "test_registration_reject() failed: original and decoded messages do not match\n");
  free_fgs_registration_reject(&dec);
}

/** @brief Test NAS Authentication Failure enc/dec */
static void test_auth_failure(void)
{
  // Dummy NAS Authentication Failure message
  uint8_t afp[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  const fgmm_auth_failure_t orig = {
      .cause = Synch_failure,
      .authentication_failure_param.len = 6,
      .authentication_failure_param.buf = afp,
  };

  // Expected encoded data
  uint8_t expected_enc[] = {0x15, 0x30, 0x06, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

  // Buffer
  uint8_t buf[16] = {0};
  byte_array_t buffer = {.buf = buf, .len = sizeof(buf)};

  // Encode
  int encoded_length = encode_fgmm_auth_failure(&buffer, &orig);
  AssertFatal(encoded_length == sizeofArray(expected_enc), "encode_fgmm_auth_failure() failed\n");

  // Compare the raw encoded buffer with expected encoded data
  AssertFatal(memcmp(buffer.buf, expected_enc, encoded_length) == 0, "Encoding mismatch!\n");

  // Decode
  fgmm_auth_failure_t dec = {0};
  int decoded_length = decode_fgmm_auth_failure(&dec, &buffer);
  AssertFatal(decoded_length >= 0, "decode_fgmm_auth_failure() failed\n");

  // Compare original and decoded messages
  AssertFatal(eq_fgmm_auth_failure(&orig, &dec), "test_auth_failure() failed: original and decoded messages do not match\n");
  free_fgmm_auth_failure(&dec);
}

/** @brief Test NAS Security Mode Reject enc/dec */
static void test_security_mode_reject(void)
{
  // Dummy NAS Security Mode Reject message
  const fgs_security_mode_reject_msg orig = {
      .cause = Illegal_UE,
  };

  // Expected encoded data
  uint8_t expected_enc[] = {0x03};

  // Buffer
  uint8_t buf[8] = {0};
  byte_array_t buffer = {.buf = buf, .len = sizeof(buf)};

  // Encode
  int encoded_length = encode_fgs_security_mode_reject(&buffer, &orig);
  AssertFatal(encoded_length == sizeofArray(expected_enc), "encode_fgs_security_mode_reject() failed\n");

  // Compare the raw encoded buffer with expected encoded data
  AssertFatal(memcmp(buffer.buf, expected_enc, encoded_length) == 0, "Encoding mismatch!\n");

  // Decode
  fgs_security_mode_reject_msg dec = {0};
  int decoded_length = decode_fgs_security_mode_reject(&dec, &buffer);
  AssertFatal(decoded_length >= 0, "decode_fgs_security_mode_reject() failed\n");

  // Compare original and decoded messages
  AssertFatal(eq_sec_mode_reject(&orig, &dec), "test_sec_mode_reject() failed: original and decoded messages do not match\n");
}

/**
 * @brief Test NAS Authentication Reject enc/dec
 */
static void test_auth_reject(void)
{
  // Dummy NAS Authentication Reject message
  uint8_t dummy_eap_msg[] = {0x12, 0x34, 0x56, 0x78, 0x91, 0x01, 0x23}; // Example EAP message
  fgmm_auth_reject_msg_t orig = {
      .eap_msg.len = sizeof(dummy_eap_msg),
  };
  orig.eap_msg.buf = dummy_eap_msg;

  // Expected encoded data
  uint8_t expected_enc[] = {0x78, 0x00, 0x07, 0x12, 0x34, 0x56, 0x78, 0x91, 0x01, 0x23};

  // Buffer
  uint8_t buf[10] = {0};
  byte_array_t buffer = {.buf = buf, .len = sizeof(buf)};

  // Encode
  int encoded_length = encode_fgmm_auth_reject(&buffer, &orig);
  AssertFatal(encoded_length >= 0, "encode_fgmm_auth_reject() failed\n");

  // Compare the raw encoded buffer with expected encoded data
  AssertFatal(encoded_length == sizeof(expected_enc), "Encoded length mismatch %d != %ld \n", encoded_length, sizeof(expected_enc));
  AssertFatal(memcmp(buffer.buf, expected_enc, encoded_length) == 0, "Encoding mismatch!\n");

  // Decode
  fgmm_auth_reject_msg_t dec = {0};
  uint8_t eap_msg[1500] = {0};
  dec.eap_msg.buf = eap_msg;
  int decoded_length = decode_fgmm_auth_reject(&dec, &buffer);
  AssertFatal(decoded_length >= 0, "decode_fgmm_auth_reject() failed\n");

  // Compare original and decoded messages
  AssertFatal(eq_auth_reject(&orig, &dec), "test_auth_reject() failed: original and decoded messages do not match\n");
}

/** @brief Test the plain 5GMM header enc/dec and the 5GSM header decoder; decoding must fail on a truncated header */
static void test_msg_headers(void)
{
  const fgmm_msg_header_t mm_orig = {
      .ex_protocol_discriminator = FGS_MOBILITY_MANAGEMENT_MESSAGE,
      .security_header_type = PLAIN_5GS_MSG,
      .message_type = FGS_SERVICE_ACCEPT,
  };
  const uint8_t mm_expected[] = {FGS_MOBILITY_MANAGEMENT_MESSAGE, PLAIN_5GS_MSG, FGS_SERVICE_ACCEPT};
  uint8_t mm[sizeof(mm_expected)] = {0};
  AssertFatal(_nas_mm_msg_encode_header(&mm_orig, mm, sizeof(mm)) == sizeof(mm), "_nas_mm_msg_encode_header() failed\n");
  AssertFatal(memcmp(mm, mm_expected, sizeof(mm)) == 0, "Encoding mismatch!\n");
  fgmm_msg_header_t mm_dec = {0};
  AssertFatal(decode_5gmm_msg_header(&mm_dec, mm, sizeof(mm)) == sizeof(mm), "decode_5gmm_msg_header() failed\n");
  AssertFatal(mm_dec.ex_protocol_discriminator == mm_orig.ex_protocol_discriminator
                  && mm_dec.security_header_type == mm_orig.security_header_type && mm_dec.message_type == mm_orig.message_type,
              "5GMM header: original and decoded headers do not match\n");
  AssertFatal(decode_5gmm_msg_header(&mm_dec, mm, sizeof(mm) - 1) < 0, "decode_5gmm_msg_header() accepted a truncated header\n");

  const uint8_t sm[] = {FGS_SESSION_MANAGEMENT_MESSAGE, 1, 0, FGS_PDU_SESSION_ESTABLISHMENT_ACC};
  fgsm_msg_header_t sm_header = {0};
  AssertFatal(decode_5gsm_msg_header(&sm_header, sm, sizeof(sm)) == sizeof(sm)
                  && sm_header.message_type == FGS_PDU_SESSION_ESTABLISHMENT_ACC,
              "decode_5gsm_msg_header() failed\n");
  AssertFatal(decode_5gsm_msg_header(&sm_header, sm, sizeof(sm) - 1) < 0, "decode_5gsm_msg_header() accepted a truncated header\n");
}

/** @brief The Requested MBS container the UE sends to join a multicast MBS session
 *         (TS 24.501 clause 9.11.4.30), and the whole PDU Session Modification Request around it
 *         (clause 8.3.7). Checked against the octets the clause prescribes, because the SMF decodes
 *         them by offset and a wrong bit order is not visible in any log. */
static void test_mbs_join_request(void)
{
  requested_mbs_info_t info = {.operation = MBS_OP_JOIN, .tmgi = {0xab, 0xcd, 0xef, 0x99, 0xf9, 0x07}};
  uint8_t buf[64] = {0};

  /* octet 4: bits 1-2 = ID type 01 (TMGI), bits 3-4 = operation 01 (join) -> 0x05 */
  int len = encode_requested_mbs_container(buf, sizeof(buf), &info, 1);
  AssertFatal(len == 7, "Requested MBS container: expected 7 octets, got %d\n", len);
  AssertFatal(buf[0] == 0x05, "Requested MBS container: octet 4 is 0x%02x, expected 0x05\n", buf[0]);
  AssertFatal(memcmp(buf + 1, info.tmgi, MBS_TMGI_LEN) == 0, "Requested MBS container: wrong TMGI\n");

  /* a leave is operation 10 -> 0x09 */
  info.operation = MBS_OP_LEAVE;
  AssertFatal(encode_requested_mbs_container(buf, sizeof(buf), &info, 1) == 7, "encode failed\n");
  AssertFatal(buf[0] == 0x09, "Requested MBS container: a leave is 0x%02x, expected 0x09\n", buf[0]);

  /* it refuses to write past the buffer rather than truncating the IE */
  AssertFatal(encode_requested_mbs_container(buf, 6, &info, 1) == -1, "encode accepted a short buffer\n");

  /* the whole message: 4 octets of 5GSM header, then the container as a type 6 IE (IEI, 2 octets of
   * length, contents) */
  pdu_session_modification_request_msg msg = {
      .protocoldiscriminator = FGS_SESSION_MANAGEMENT_MESSAGE,
      .pdusessionid = 1,
      .pti = 1,
      .pdusessionmodificationrequestmsgtype = FGS_PDU_SESSION_MODIFICATION_REQ,
      .requested_mbs_present = true,
      .num_requested_mbs = 1,
  };
  msg.requested_mbs[0].operation = MBS_OP_JOIN;
  memcpy(msg.requested_mbs[0].tmgi, info.tmgi, MBS_TMGI_LEN);

  const uint8_t expected[] = {
      FGS_SESSION_MANAGEMENT_MESSAGE, 1, 1, FGS_PDU_SESSION_MODIFICATION_REQ,
      0x70, 0x00, 0x07,
      0x05, 0xab, 0xcd, 0xef, 0x99, 0xf9, 0x07,
  };
  len = encode_pdu_session_modification_request(&msg, buf, sizeof(buf));
  AssertFatal(len == (int)sizeof(expected), "Modification Request: %d octets, expected %zu\n", len, sizeof(expected));
  AssertFatal(memcmp(buf, expected, sizeof(expected)) == 0, "Modification Request: encoding mismatch\n");
}

/** @brief The Received MBS container the SMF answers with (TS 24.501 clause 9.11.4.31), decoded out
 *         of a PDU Session Modification Command. */
static void test_mbs_join_answer(void)
{
  received_mbs_container_t out = {0};

  /* accepted: MD = 010, nothing optional */
  {
    const uint8_t buf[] = {0x02, 0x00, 0xab, 0xcd, 0xef, 0x99, 0xf9, 0x07};
    AssertFatal(decode_received_mbs_container(&out, buf, sizeof(buf)) == (int)sizeof(buf), "decode failed\n");
    AssertFatal(out.num == 1, "expected 1 entry, got %d\n", out.num);
    AssertFatal(out.info[0].decision == MBS_DECISION_JOIN_ACCEPTED, "expected accepted, got %d\n", out.info[0].decision);
    AssertFatal(memcmp(out.info[0].tmgi, buf + 2, MBS_TMGI_LEN) == 0, "wrong TMGI\n");
  }

  /* rejected with "session context not found": MD = 011, cause = 101 in bits 6-8 */
  {
    const uint8_t buf[] = {(5 << 5) | 3, 0x00, 0x00, 0x00, 0x01, 0x99, 0xf9, 0x07};
    AssertFatal(decode_received_mbs_container(&out, buf, sizeof(buf)) == (int)sizeof(buf), "decode failed\n");
    AssertFatal(out.info[0].decision == MBS_DECISION_JOIN_REJECTED, "expected rejected\n");
    AssertFatal(out.info[0].reject_cause == MBS_REJECT_CONTEXT_NOT_FOUND, "wrong rejection cause\n");
  }

  /* two entries back to back */
  {
    const uint8_t buf[] = {
        0x02, 0x00, 0x00, 0x00, 0x01, 0x99, 0xf9, 0x07,
        (2 << 5) | 3, 0x00, 0x00, 0x00, 0x02, 0x99, 0xf9, 0x07,
    };
    AssertFatal(decode_received_mbs_container(&out, buf, sizeof(buf)) == (int)sizeof(buf), "decode failed\n");
    AssertFatal(out.num == 2, "expected 2 entries, got %d\n", out.num);
    AssertFatal(out.info[1].reject_cause == MBS_REJECT_NOT_AUTHORIZED, "wrong rejection cause\n");
    AssertFatal(out.info[1].tmgi[2] == 0x02, "wrong TMGI in the second entry\n");
  }

  /* the optional fields are skipped over, so the entry after them is still found: IPAE=1 with IPv4
   * (8 octets of addresses) and an MBS back-off timer (MTI=10, 1 octet) */
  {
    const uint8_t buf[] = {
        0x02, 0x01 | (2 << 1), 0x00, 0x00, 0x01, 0x99, 0xf9, 0x07,
        192, 168, 77, 20, 239, 0, 0, 25,
        0x3c,
        0x02, 0x00, 0x00, 0x00, 0x02, 0x99, 0xf9, 0x07,
    };
    AssertFatal(decode_received_mbs_container(&out, buf, sizeof(buf)) == (int)sizeof(buf), "decode failed\n");
    AssertFatal(out.num == 2, "expected 2 entries past the optional fields, got %d\n", out.num);
    AssertFatal(out.info[1].tmgi[2] == 0x02, "wrong TMGI in the second entry\n");
  }

  /* a truncated entry is a malformed IE, not a short read */
  {
    const uint8_t buf[] = {0x02, 0x00, 0xab, 0xcd};
    AssertFatal(decode_received_mbs_container(&out, buf, sizeof(buf)) == -1, "a truncated container was accepted\n");
  }

  /* an entry whose optional fields run past the IE is refused too */
  {
    const uint8_t buf[] = {0x02, 0x01, 0x00, 0x00, 0x01, 0x99, 0xf9, 0x07, 1, 2};
    AssertFatal(decode_received_mbs_container(&out, buf, sizeof(buf)) == -1, "an overrunning container was accepted\n");
  }

  /* and the IE in its message: a PDU Session Modification Command whose only IE is 0x71 */
  {
    const uint8_t cmd[] = {0x71, 0x00, 0x08, 0x02, 0x00, 0xab, 0xcd, 0xef, 0x99, 0xf9, 0x07};
    pdu_session_modification_command_msg_t msg = {0};
    AssertFatal(decode_pdu_session_modification_command(&msg, (uint8_t *)cmd, sizeof(cmd)) == (int)sizeof(cmd),
                "decode_pdu_session_modification_command() failed\n");
    AssertFatal(msg.received_mbs_present, "the Received MBS container was not reported\n");
    AssertFatal(msg.received_mbs.num == 1 && msg.received_mbs.info[0].decision == MBS_DECISION_JOIN_ACCEPTED,
                "the Received MBS container was not decoded\n");
  }
}

/** @brief --mbs-join takes 12 hex characters of TMGI, of either case. */
static void test_mbs_tmgi_from_string(void)
{
  uint8_t tmgi[MBS_TMGI_LEN] = {0};
  const uint8_t expected[MBS_TMGI_LEN] = {0xab, 0xcd, 0xef, 0x99, 0xf9, 0x07};

  AssertFatal(mbs_tmgi_from_string("abcdef99f907", tmgi), "a lowercase TMGI was refused\n");
  AssertFatal(memcmp(tmgi, expected, MBS_TMGI_LEN) == 0, "wrong octets from a lowercase TMGI\n");
  AssertFatal(mbs_tmgi_from_string("ABCDEF99F907", tmgi), "an uppercase TMGI was refused\n");
  AssertFatal(memcmp(tmgi, expected, MBS_TMGI_LEN) == 0, "wrong octets from an uppercase TMGI\n");
  AssertFatal(!mbs_tmgi_from_string("abcdef99f90", tmgi), "a short TMGI was accepted\n");
  AssertFatal(!mbs_tmgi_from_string("abcdef99f9077", tmgi), "a long TMGI was accepted\n");
  AssertFatal(!mbs_tmgi_from_string("abcdef99f90g", tmgi), "a non-hex TMGI was accepted\n");
  AssertFatal(!mbs_tmgi_from_string(NULL, tmgi), "NULL was accepted\n");
}

int main()
{
  // Initialize logging system
  logInit();
  // Set logging level for NAS to show INFO messages
  set_log(NAS, OAILOG_DEBUG);
  // Tests list
  test_regression_registration_accept();
  test_service_request();
  test_service_accept();
  test_service_reject();
  test_registration_reject();
  test_auth_failure();
  test_auth_reject();
  test_security_mode_reject();
  test_msg_headers();
  test_mbs_join_request();
  test_mbs_join_answer();
  test_mbs_tmgi_from_string();
  return 0;
}
