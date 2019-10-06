/*
 * Copyright (C) 2018-2019
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v3.0. See the file LICENSE in the top level
 * directory for more details.
 *
 * See AUTHORS.md for complete list of NDN IOT PKG authors and contributors.
 */

#include "pub-sub.h"
#include "service-discovery.h"
#include "../encode/key-storage.h"
#include "../encode/wrapper-api.h"

#define NDN_PUBSUB_TOPIC_SIZE 10
#define NDN_PUBSUB_IDENTIFIER_SIZE 2


// the struct to keep each topic subscribed
typedef struct topic {
  uint8_t service;
  name_component_t identifier[NDN_PUBSUB_IDENTIFIER_SIZE];
  uint32_t identifier_size;
  uint32_t interval; // the time interval between two Interests
  uint32_t next_interest; // the time to send next Interest
  ndn_on_content_published callback;
  
  uint8_t is_sub;
  uint8_t cache[200];
  uint32_t cache_size;
} topic_t;

// the struct to keep registered topics, including: service, identifer (name_components), frequency, and callback
typedef struct sub_topics {
  topic_t topics[NDN_PUBSUB_TOPIC_SIZE];
  uint32_t min_interval;
} sub_topics_t;

static sub_topics_t m_sub_state;
static bool m_has_initialized = false;

topic_t*
_match_topic(const ndn_name_t* data_name, uint8_t is_sub)
{
  // match the topics and load the matched topic into the topic
  /*
    /<homeprefix>/<service>/<identifier>/<locator>
  */

  printf("_match_topic, data_name = ");ndn_name_print(data_name);putchar('\n');

  ndn_name_t prefix;
  ndn_key_storage_t* storage = ndn_key_storage_get_instance();
  name_component_t* home_prefix = &storage->self_identity.components[0];
  for (int i = 0; i < NDN_PUBSUB_TOPIC_SIZE; i++) 
  {
    if (m_sub_state.topics[i].identifier_size != NDN_FWD_INVALID_NAME_SIZE && 
        m_sub_state.topics[i].is_sub == is_sub)
    {
      ndn_name_init(&prefix);
      ndn_name_append_component(&prefix, home_prefix);
      ndn_name_append_bytes_component(&prefix, &m_sub_state.topics[i].service,
                                       sizeof(m_sub_state.topics[i].service));
      for (int j = 0; j < m_sub_state.topics[i].identifier_size; j++)
        ndn_name_append_component(&prefix, &m_sub_state.topics[i].identifier[j]);
      
      // compare against data_name
      printf("_match_topic, to compare prefix = ");ndn_name_print(&prefix);putchar('\n');
      int ret = ndn_name_is_prefix_of(&prefix, data_name);
      if (!ret) {
        printf("_match_topic, prefix matched!\n");
        return &m_sub_state.topics[i];
      }
    }
  }
  return NULL;
}

void
_on_new_content(const uint8_t* raw_data, uint32_t data_size, void* userdata)
{
  // parse Data name
  ndn_name_t data_name;
  uint8_t* content;
  size_t content_size;

  tlv_parse_data(raw_data, data_size, 3, 
                 TLV_DATAARG_NAME_PTR, &data_name,
                 TLV_DATAARG_CONTENT_BUF, &content,
                 TLV_DATAARG_CONTENT_SIZE, &content_size);
  
  // match the subscription topic
  topic_t* entry = _match_topic(&data_name, 1);
  if (!entry) {
    printf("_on_new_content: no matching topic, discard\n");
    return;
  } 

  printf("_on_new_content: in coming\n");

  // call the on_content callback
  // TODO: use sig_verifier
  entry->callback(entry->service, 0, &entry->identifier, entry->identifier_size, 
                  content, content_size);
}

void
_periodic_data_fetching(void *self, size_t param_length, void *param)
{
  (void)self;(void)param_length;(void)param;
  ndn_time_ms_t now = ndn_time_now_ms();
  ndn_encoder_t encoder;
  
  // check the table
  for (int i = 0; i < NDN_PUBSUB_TOPIC_SIZE; i++) {
    if (m_sub_state.topics[i].identifier_size != NDN_FWD_INVALID_NAME_SIZE
       && m_sub_state.topics[i].is_sub == 1)
    {
      if (now > m_sub_state.topics[i].next_interest)
      {
        int ret = ndn_forwarder_express_interest(m_sub_state.topics[i].cache, 
                                                 m_sub_state.topics[i].cache_size,
                                                 _on_new_content, NULL, NULL);
        
        ndn_name_t print;
        ndn_name_init(&print);
        tlv_parse_interest(m_sub_state.topics[i].cache, m_sub_state.topics[i].cache_size, 1,
                           TLV_INTARG_NAME_PTR, &print);
        printf("_periodic_data_fetching: ");
        ndn_name_print(&print);putchar('\n');
        m_sub_state.topics[i].next_interest = now + m_sub_state.topics[i].interval;
      }
    }
  }

  // register the event to the message queue again with the smallest time period
  ndn_msgqueue_post(NULL, _periodic_data_fetching, 0, NULL);
}

void
_on_notification_interest(const uint8_t* raw_interest, uint32_t interest_size, void* userdata)
{
  // parse notification interest
  // match the topic
  // send the Interest to fetch new content
}

int
_on_subscription_interest(const uint8_t* raw_interest, uint32_t interest_size, void* userdata)
{
  // parse interest
  printf("_on_subscription_interest\n");
  ndn_name_t interest_name;
  tlv_parse_interest(raw_interest, interest_size, 1, 
                     TLV_INTARG_NAME_PTR, &interest_name);
  // match topic
  topic_t* entry = _match_topic(&interest_name, 0);

  // reply the latest content
  if (entry)
  {
    printf("_on_subscription_interest: put data\n");
    ndn_forwarder_put_data(entry->cache, entry->cache_size);
  }
  return NDN_FWD_STRATEGY_MULTICAST;
}


void
_ps_topics_init()
{
  for (int i = 0; i < NDN_PUBSUB_TOPIC_SIZE; i++) {
    m_sub_state.topics[i].identifier_size = NDN_FWD_INVALID_NAME_SIZE;
    m_sub_state.topics[i].callback = NULL;
    m_sub_state.topics[i].next_interest = 0;
  }
  m_has_initialized = true;
}

/* 
E.g., LED => /home/LED
E.g., TEMP, bedroom => /home/TEMP/bedroom
*/
void
ps_subscribe_to(uint8_t service, const name_component_t* identifier, uint32_t component_size,
                uint32_t frequency, ndn_on_content_published callback)
{
  if (!m_has_initialized) 
    _ps_topics_init();

  int ret = 0;
  ndn_name_t name;
  ndn_name_init(&name);
  ndn_key_storage_t* storage = ndn_key_storage_get_instance();

  name_component_t* home_prefix = &storage->self_identity.components[0];
  ret = ndn_name_append_component(&name, home_prefix);
  if (ret) return;

  if (service) {
    ret = ndn_name_append_bytes_component(&name, &service, sizeof(service));
    if (ret) return;
  }

  if(identifier) {
    for (int i = 0; i < component_size; i++) {
      ret = ndn_name_append_component(&name, &identifier[i]);
      if (ret) return;
    }
  }
  
  printf("before finding entry ");ndn_name_print(&name);putchar('\n');
    // retrieve an topic entry
  // TODO need removing mechanism
  topic_t* entry = NULL;
  for (int i = 0; i < NDN_PUBSUB_TOPIC_SIZE; i++) {
    if (m_sub_state.topics[i].identifier_size == NDN_FWD_INVALID_NAME_SIZE)
      m_sub_state.topics[i].callback = callback;
      m_sub_state.topics[i].interval = frequency;
      m_sub_state.topics[i].service = service;
      m_sub_state.topics[i].is_sub = 1;
      //immediate send
      m_sub_state.topics[i].next_interest = (uint32_t)ndn_time_now_ms();
      if (identifier && component_size <= NDN_PUBSUB_IDENTIFIER_SIZE) {
        for (int j = 0; j < NDN_PUBSUB_IDENTIFIER_SIZE; j++)
          m_sub_state.topics[i].identifier[j] = identifier[j];
        m_sub_state.topics[i].identifier_size = component_size;
      }
      entry = &m_sub_state.topics[i];
      printf("_ps_subscribe_to, find an empty entry!\n");
      break;
  }
  
  tlv_make_interest(&entry->cache, sizeof(entry->cache), &entry->cache_size, 4,
                    TLV_INTARG_NAME_PTR, &name,
                    TLV_INTARG_CANBEPREFIX_BOOL, true,
                    TLV_INTARG_MUSTBEFRESH_BOOL, true,
                    TLV_INTARG_LIFETIME_U64, (uint64_t)600);
  printf("ps_subscribe_to: encoded interest cache size = %d\n", entry->cache_size);
  _periodic_data_fetching(NULL, 0, NULL);
  return;
}

/*
Interface: service, [optional]datatype, content
E.g., TEMP, bedroom, “37degree” => /home/TEMP/bedroom/sensor-1
*/
void
ps_publish_content(uint8_t service, const name_component_t* identifier, uint32_t component_size,
                   uint8_t* content, uint32_t content_len)
{
  if (!m_has_initialized)
    _ps_topics_init();
  
  int ret = 0;
  ndn_name_t name;
  ndn_name_init(&name);
  ndn_key_storage_t* storage = ndn_key_storage_get_instance();
  name_component_t* home_prefix = &storage->self_identity.components[0];
  
  ret = ndn_name_append_component(&name, home_prefix);
  if (ret) return;
  ret = ndn_name_append_bytes_component(&name, &service, sizeof(service));
  if (ret) return;

  // register prefix
  ndn_forwarder_register_name_prefix(&name, _on_subscription_interest, NULL);
  printf("ps_publish_content: registering prefix ");ndn_name_print(&name);putchar('\n');

  if(identifier) {
    for (int i = 0; i < component_size; i++) {
      ret = ndn_name_append_component(&name, &identifier[i]);
      if (ret) return;
    }
  }

  for (int i = 0; i < storage->self_identity.components_size - 1; i++)
  {
    ret = ndn_name_append_component(&name, &storage->self_identity.components[i + 1]);
    if (ret) return;
  }

  topic_t* entry = NULL;
  for (int i = 0; i < NDN_PUBSUB_TOPIC_SIZE; i++) {
    if (m_sub_state.topics[i].identifier_size == NDN_FWD_INVALID_NAME_SIZE)
      m_sub_state.topics[i].service = service;
      m_sub_state.topics[i].is_sub = 0;
      if (identifier && component_size <= NDN_PUBSUB_IDENTIFIER_SIZE) {
        for (int j = 0; j < NDN_PUBSUB_IDENTIFIER_SIZE; j++)
          m_sub_state.topics[i].identifier[j] = identifier[j];
        m_sub_state.topics[i].identifier_size = component_size;
      }
      entry = &m_sub_state.topics[i];
      printf("ps_publish_content: got an entry\n");
      break;
  }

  ret = tlv_make_data(entry->cache, sizeof(entry->cache), &entry->cache_size, 6,
                      TLV_DATAARG_NAME_PTR, &name,
                      TLV_DATAARG_CONTENT_BUF, content,
                      TLV_DATAARG_CONTENT_SIZE, content_len,
                      TLV_DATAARG_SIGTYPE_U8, NDN_SIG_TYPE_ECDSA_SHA256,
                      TLV_DATAARG_IDENTITYNAME_PTR, &storage->self_identity,
                      TLV_DATAARG_SIGKEY_PTR, &storage->self_identity_key);

  printf("ps_publish_content: data name:\n");ndn_name_print(&name);putchar('\n');
  if (ret) return;

  return; 
}

void
ps_publish_command(uint8_t service, uint16_t action, const name_component_t* identifier, uint32_t component_size,
                   uint8_t* content, uint32_t content_len)
{

}
