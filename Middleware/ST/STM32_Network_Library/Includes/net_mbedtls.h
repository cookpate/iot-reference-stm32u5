/**
  ******************************************************************************
  * @file    net_mbedtls.h
  * @author  MCD Application Team
  * @brief   Header for the network TLS functions.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef MBEDTLS_NET_H
#define MBEDTLS_NET_H

/* Includes ------------------------------------------------------------------*/
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/certs.h"
#include "mbedtls/x509.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"
#include "mbedtls/timing.h"


/* Private defines -----------------------------------------------------------*/

struct net_tls_data
{
  const char_t *tls_ca_certs;   /**< Socket option. */
  const char_t *tls_ca_crl;     /**< Socket option. */
  const char_t *tls_dev_cert;   /**< Socket option. */
  const char_t *tls_dev_key;    /**< Socket option. */
  const uint8_t *tls_dev_pwd;   /**< Socket option. */
  size_t tls_dev_pwd_len;       /**< Socket option / meta. */
  bool tls_srv_verification;    /**< Socket option. */
  const char_t *tls_srv_name;   /**< Socket option. */
  /* mbedTLS objects */
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  uint32_t flags;
  mbedtls_x509_crt cacert;
  mbedtls_x509_crt clicert;
  mbedtls_pk_context pkey;
  const mbedtls_x509_crt_profile *tls_cert_prof;  /**< Socket option. */
} ;

void net_tls_init(void);
void net_tls_destroy(void);

int32_t net_mbedtls_start(net_socket_t *sockhnd);
int32_t net_mbedtls_stop(net_socket_t *sockhnd);
int32_t net_mbedtls_sock_recv(net_socket_t *sockhnd, uint8_t *buf, size_t len);
int32_t net_mbedtls_sock_send(net_socket_t *sockhnd, const uint8_t *buf, size_t len);
bool net_mbedtls_check_tlsdata(net_socket_t *sockhnd);
void net_mbedtls_set_read_timeout(net_socket_t *sock);

#endif /* MBEDTLS_NET_H */
