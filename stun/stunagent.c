/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2007 Nokia Corporation. All rights reserved.
 *  Contact: Rémi Denis-Courmont
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Nice GLib ICE library.
 *
 * The Initial Developers of the Original Code are Collabora Ltd and Nokia
 * Corporation. All Rights Reserved.
 *
 * Contributors:
 *   Rémi Denis-Courmont, Nokia
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * the GNU Lesser General Public License Version 2.1 (the "LGPL"), in which
 * case the provisions of LGPL are applicable instead of those above. If you
 * wish to allow use of your version of this file only under the terms of the
 * LGPL and not to allow others to use your version of this file under the
 * MPL, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the LGPL. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under either the MPL or the LGPL.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>

#include "stunmessage.h"

#include <string.h>
#include <stdlib.h>

#include <errno.h>
#include <netinet/in.h>


static bool stun_agent_is_unknown (StunAgent *agent, uint16_t type);
static unsigned stun_agent_find_unknowns (StunAgent *agent,
    const StunMessage * msg, uint16_t *list, unsigned max);

void stun_agent_init (StunAgent *agent, const uint16_t *known_attributes,
    StunCompatibility compatibility, uint32_t usage_flags)
{
  int i;

  agent->known_attributes = (uint16_t *) known_attributes;
  agent->compatibility = compatibility;
  agent->usage_flags = usage_flags;

  for (i = 0; i < STUN_AGENT_MAX_SAVED_IDS; i++) {
    agent->sent_ids[i].valid = FALSE;
  }
}


bool stun_agent_default_validater (StunAgent *agent,
    StunMessage *message, uint8_t *username, uint16_t username_len,
    uint8_t **password, size_t *password_len, void *user_data)
{
  stun_validater_data* val = (stun_validater_data*) user_data;
  int i;

  for (i = 0; val && val[i].username ; i++) {
    if (username_len == val[i].username_len ||
        memcmp (username, val[i].username, username_len) == 0) {
      *password = (uint8_t *) val[i].password;
      *password_len = val[i].password_len;

      return true;
    }
  }

  return false;

}

StunValidationStatus stun_agent_validate (StunAgent *agent, StunMessage *msg,
    const uint8_t *buffer, size_t buffer_len,
    StunMessageIntegrityValidate validater, void * validater_data)
{
  stun_transid_t msg_id;
  uint32_t fpr;
  uint32_t crc32;
  int len;
  uint8_t *username = NULL;
  uint16_t username_len;
  uint8_t *key = NULL;
  size_t key_len;
  uint8_t *hash;
  uint8_t sha[20];
  uint16_t hlen;
  int sent_id_idx = -1;
  uint16_t unknown;

  len = stun_message_validate_buffer_length (buffer, buffer_len);
  if (len == STUN_MESSAGE_BUFFER_INVALID) {
    return STUN_VALIDATION_NOT_STUN;
  } else if (len == STUN_MESSAGE_BUFFER_INCOMPLETE) {
    return STUN_VALIDATION_INCOMPLETE_STUN;
  } else if (len != (int) buffer_len) {
    return STUN_VALIDATION_NOT_STUN;
  }

  msg->buffer = (uint8_t *) buffer;
  msg->buffer_len = buffer_len;
  msg->agent = agent;
  msg->key = NULL;
  msg->key_len = 0;

  /* TODO: reject it or not ? */
  if (agent->compatibility == STUN_COMPATIBILITY_3489BIS &&
      !stun_has_cookie (msg)) {
      stun_debug ("STUN demux error: no cookie!\n");
      return STUN_VALIDATION_BAD_REQUEST;
  }

  if (agent->compatibility == STUN_COMPATIBILITY_3489BIS &&
      agent->usage_flags & STUN_AGENT_USAGE_USE_FINGERPRINT) {
    /* Looks for FINGERPRINT */
    if (stun_message_find32 (msg, STUN_ATTRIBUTE_FINGERPRINT, &fpr) != 0) {
      stun_debug ("STUN demux error: no FINGERPRINT attribute!\n");
      return STUN_VALIDATION_BAD_REQUEST;
    }

    /* Checks FINGERPRINT */
    crc32 = stun_fingerprint (msg->buffer, stun_message_length (msg));
    fpr = ntohl (fpr);
    if (fpr != crc32) {
      stun_debug ("STUN demux error: bad fingerprint: 0x%08x,"
          " expected: 0x%08x!\n", fpr, crc32);
      return STUN_VALIDATION_BAD_REQUEST;
    }

    stun_debug ("STUN demux: OK!\n");
  }

  if (stun_message_get_class (msg) == STUN_RESPONSE ||
      stun_message_get_class (msg) == STUN_ERROR) {
    stun_message_id (msg, msg_id);
    for (sent_id_idx = 0; sent_id_idx < STUN_AGENT_MAX_SAVED_IDS; sent_id_idx++) {
      if (agent->sent_ids[sent_id_idx].valid == TRUE &&
          agent->sent_ids[sent_id_idx].method == stun_message_get_method (msg) &&
          memcmp (msg_id, agent->sent_ids[sent_id_idx].id,
              sizeof(stun_transid_t)) == 0) {

        key = agent->sent_ids[sent_id_idx].key;
        key_len = agent->sent_ids[sent_id_idx].key_len;
        break;
      }
    }
    if (sent_id_idx == STUN_AGENT_MAX_SAVED_IDS) {
      return STUN_VALIDATION_UNMATCHED_RESPONSE;
    }
  }

  if ((agent->usage_flags & STUN_AGENT_USAGE_SHORT_TERM_CREDENTIALS &&
      (!stun_message_has_attribute (msg, STUN_ATTRIBUTE_USERNAME) ||
       !stun_message_has_attribute (msg, STUN_ATTRIBUTE_MESSAGE_INTEGRITY))) ||
      (agent->usage_flags & STUN_AGENT_USAGE_LONG_TERM_CREDENTIALS &&
       stun_message_get_class (msg) != STUN_INDICATION &&
       (!stun_message_has_attribute (msg, STUN_ATTRIBUTE_USERNAME) ||
        !stun_message_has_attribute (msg, STUN_ATTRIBUTE_MESSAGE_INTEGRITY) ||
        !stun_message_has_attribute (msg, STUN_ATTRIBUTE_NONCE) ||
        !stun_message_has_attribute (msg, STUN_ATTRIBUTE_REALM))) ||
      ((agent->usage_flags & STUN_AGENT_USAGE_IGNORE_CREDENTIALS) == 0 &&
        stun_message_has_attribute (msg, STUN_ATTRIBUTE_USERNAME) &&
        !stun_message_has_attribute (msg, STUN_ATTRIBUTE_MESSAGE_INTEGRITY))) {
        return STUN_VALIDATION_UNAUTHORIZED;
  }

  if ((agent->usage_flags & STUN_AGENT_USAGE_IGNORE_CREDENTIALS) == 0 &&
      stun_message_has_attribute (msg, STUN_ATTRIBUTE_MESSAGE_INTEGRITY)) {
    username_len = 0;
    username = (uint8_t *) stun_message_find (msg, STUN_ATTRIBUTE_USERNAME,
        &username_len);
    if (key == NULL) {
      if (validater == NULL ||
          validater (agent, msg, username, username_len,
              &key, &key_len, validater_data) == FALSE) {
        return STUN_VALIDATION_UNAUTHORIZED;
      }
    }
  }

  if (key != NULL && key_len > 0) {
    hash = (uint8_t *) stun_message_find (msg,
        STUN_ATTRIBUTE_MESSAGE_INTEGRITY, &hlen);

    /* We must give the size from start to the end of the attribute
       because you might have a FINGERPRINT attribute after it... */
    stun_sha1 (msg->buffer, hash + 20 - msg->buffer, sha, key, key_len);
    stun_debug (" Message HMAC-SHA1 fingerprint:");
    stun_debug ("\nkey     : ");
    stun_debug_bytes (key, key_len);
    stun_debug ("\n  expected: ");
    stun_debug_bytes (sha, sizeof (sha));
    stun_debug ("\n  received: ");
    stun_debug_bytes (hash, sizeof (sha));
    stun_debug ("\n");

    if (memcmp (sha, hash, sizeof (sha)))  {
      stun_debug ("STUN auth error: SHA1 fingerprint mismatch!\n");
      return STUN_VALIDATION_UNAUTHORIZED;
    }

    stun_debug ("STUN auth: OK!\n");
    msg->key = key;
    msg->key_len = key_len;
  }


  if (sent_id_idx != -1 && sent_id_idx < STUN_AGENT_MAX_SAVED_IDS) {
    agent->sent_ids[sent_id_idx].valid = FALSE;
  }

  if (stun_agent_find_unknowns (agent, msg, &unknown, 1) > 0) {
    if (stun_message_get_class (msg) == STUN_REQUEST)
      return STUN_VALIDATION_UNKNOWN_REQUEST_ATTRIBUTE;
    else
      return STUN_VALIDATION_UNKNOWN_ATTRIBUTE;
  }
  return STUN_VALIDATION_SUCCESS;

}


bool stun_agent_init_request (StunAgent *agent, StunMessage *msg,
    uint8_t *buffer, size_t buffer_len, stun_method_t m)
{
  bool ret;
  stun_transid_t id;

  msg->buffer = buffer;
  msg->buffer_len = buffer_len;
  msg->agent = agent;
  msg->key = NULL;
  msg->key_len = 0;

  stun_make_transid (id);

  ret = stun_message_init (msg, STUN_REQUEST, m, id);

  if (ret) {
    if (agent->compatibility == STUN_COMPATIBILITY_3489BIS) {
      uint32_t cookie = htonl (STUN_MAGIC_COOKIE);
      memcpy (msg->buffer + STUN_MESSAGE_TRANS_ID_POS, &cookie, sizeof (cookie));
    }
  }

  return ret;
}


bool stun_agent_init_indication (StunAgent *agent, StunMessage *msg,
    uint8_t *buffer, size_t buffer_len, stun_method_t m)
{
  bool ret;
  stun_transid_t id;

  msg->buffer = buffer;
  msg->buffer_len = buffer_len;
  msg->agent = agent;
  msg->key = NULL;
  msg->key_len = 0;

  stun_make_transid (id);
  ret = stun_message_init (msg, STUN_INDICATION, m, id);

  if (ret) {
    if (agent->compatibility == STUN_COMPATIBILITY_3489BIS) {
      uint32_t cookie = htonl (STUN_MAGIC_COOKIE);
      memcpy (msg->buffer + STUN_MESSAGE_TRANS_ID_POS, &cookie, sizeof (cookie));
    }
  }

  return ret;
}


bool stun_agent_init_response (StunAgent *agent, StunMessage *msg,
    uint8_t *buffer, size_t buffer_len, const StunMessage *request)
{

  stun_transid_t id;

  if (stun_message_get_class (request) != STUN_REQUEST) {
    return FALSE;
  }

  msg->buffer = buffer;
  msg->buffer_len = buffer_len;
  msg->agent = agent;
  msg->key = request->key;
  msg->key_len = request->key_len;

  stun_message_id (request, id);

  if (stun_message_init (msg, STUN_RESPONSE,
          stun_message_get_method (request), id)) {

    if (agent->compatibility == STUN_COMPATIBILITY_3489BIS &&
      agent->usage_flags & STUN_AGENT_USAGE_ADD_SERVER) {
      stun_message_append_server (msg);
    }
    return TRUE;
  }
  return FALSE;
}


bool stun_agent_init_error (StunAgent *agent, StunMessage *msg,
    uint8_t *buffer, size_t buffer_len, const StunMessage *request,
    stun_error_t err)
{
  stun_transid_t id;

  if (stun_message_get_class (request) != STUN_REQUEST) {
    return FALSE;
  }

  msg->buffer = buffer;
  msg->buffer_len = buffer_len;
  msg->agent = agent;
  msg->key = request->key;
  msg->key_len = request->key_len;

  stun_message_id (request, id);


  if (stun_message_init (msg, STUN_ERROR,
          stun_message_get_method (request), id)) {

    if (agent->compatibility == STUN_COMPATIBILITY_3489BIS &&
      agent->usage_flags & STUN_AGENT_USAGE_ADD_SERVER) {
      stun_message_append_server (msg);
    }
    if (stun_message_append_error (msg, err) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}


size_t stun_agent_build_unknown_attributes_error (StunAgent *agent,
    StunMessage *msg, uint8_t *buffer, size_t buffer_len,
    const StunMessage *request)
{

  unsigned counter;
  uint16_t ids[STUN_AGENT_MAX_UNKNOWN_ATTRIBUTES];

  counter = stun_agent_find_unknowns (agent, request,
      ids, STUN_AGENT_MAX_UNKNOWN_ATTRIBUTES);

  if (stun_agent_init_error (agent, msg, buffer, buffer_len,
          request, STUN_ERROR_UNKNOWN_ATTRIBUTE) == FALSE) {
    return 0;
  }

  /* NOTE: Old RFC3489 compatibility:
   * When counter is odd, duplicate one value for 32-bits padding. */
  if (!stun_has_cookie (request) && (counter & 1))
    ids[counter++] = ids[0];

  if (stun_message_append_bytes (msg, STUN_ATTRIBUTE_UNKNOWN_ATTRIBUTES,
          ids, counter * 2) == 0) {
    return stun_agent_finish_message (agent, msg, request->key, request->key_len);
  }

  return 0;
}


size_t stun_agent_finish_message (StunAgent *agent, StunMessage *msg,
    const uint8_t *key, size_t key_len)
{
  uint8_t *ptr;
  uint32_t fpr;
  int i;
  stun_transid_t id;

  if (msg->key != NULL) {
    key = msg->key;
    key_len = msg->key_len;
  }

  if (key != NULL) {
    ptr = stun_message_append (msg, STUN_ATTRIBUTE_MESSAGE_INTEGRITY, 20);
    if (ptr == NULL) {
      return 0;
    }

    stun_sha1 (msg->buffer, stun_message_length (msg), ptr, key, key_len);

    stun_debug (" Message HMAC-SHA1 message integrity:"
         "\n  key     : ");
    stun_debug_bytes (key, key_len);
    stun_debug ("\n  sent    : ");
    stun_debug_bytes (ptr, 20);
    stun_debug ("\n");

  }

  if (agent->compatibility == STUN_COMPATIBILITY_3489BIS &&
      agent->usage_flags & STUN_AGENT_USAGE_USE_FINGERPRINT) {
    ptr = stun_message_append (msg, STUN_ATTRIBUTE_FINGERPRINT, 4);
    if (ptr == NULL) {
      return 0;
    }


    fpr = stun_fingerprint (msg->buffer, stun_message_length (msg));
    memcpy (ptr, &fpr, sizeof (fpr));

    stun_debug (" Message HMAC-SHA1 fingerprint: ");
    stun_debug_bytes (ptr, 4);
    stun_debug ("\n");
  }


  if (stun_message_get_class (msg) == STUN_REQUEST) {
    for (i = 0; i < STUN_AGENT_MAX_SAVED_IDS; i++) {
      if (agent->sent_ids[i].valid == FALSE) {
        stun_message_id (msg, id);
        memcpy (agent->sent_ids[i].id, id, sizeof(stun_transid_t));
        agent->sent_ids[i].method = stun_message_get_method (msg);
        agent->sent_ids[i].key = (uint8_t *) key;
        agent->sent_ids[i].key_len = key_len;
        agent->sent_ids[i].valid = TRUE;
        break;
      }
    }
  }

  msg->key = (uint8_t *) key;
  msg->key_len = key_len;
  return stun_message_length (msg);

}

static bool stun_agent_is_unknown (StunAgent *agent, uint16_t type)
{

  uint16_t *known_attr = agent->known_attributes;

  while(*known_attr != 0) {
    if (*known_attr == type) {
      return FALSE;
    }
    known_attr++;
  }

  return TRUE;

}


static unsigned
stun_agent_find_unknowns (StunAgent *agent, const StunMessage * msg,
    uint16_t *list, unsigned max)
{
  unsigned count = 0;
  uint16_t len = stun_message_length (msg);
  size_t offset = 0;

  offset = STUN_MESSAGE_ATTRIBUTES_POS;

  while ((offset < len) && (count < max))
  {
    size_t alen = stun_getw (msg->buffer + offset + STUN_ATTRIBUTE_TYPE_LEN);
    uint16_t atype = stun_getw (msg->buffer + offset);

    offset += STUN_ATTRIBUTE_VALUE_POS + stun_align (alen);

    if (!stun_optional (atype) && stun_agent_is_unknown (agent, atype))
    {
      stun_debug ("STUN unknown: attribute 0x%04x(%u bytes)\n",
           (unsigned)atype, (unsigned)alen);
      list[count++] = htons (atype);
    }
  }

  stun_debug ("STUN unknown: %u mandatory attribute(s)!\n", count);
  return count;
}