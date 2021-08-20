/*
 * FreeRTOS STM32 Reference Integration
 * Copyright (C) 2020-2021 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file mbedtls_transport.c
 * @brief TLS transport interface implementations using mbedtls.
 */
#include "logging_levels.h"

#define LOG_LEVEL LOG_ERROR

#include "logging.h"

#include "transport_interface_ext.h"
#include "mbedtls_transport.h"
#include <string.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"

/* mbedTLS includes. */
#include "mbedtls/error.h"
#include "mbedtls_config.h"
#include "mbedtls/debug.h"
#include "mbedtls/net_sockets.h"

#define MBEDTLS_DEBUG_THRESHOLD 1

/**
 * @brief Secured connection context.
 */
typedef struct TLSContext
{
    mbedtls_ssl_config xSslConfig;              /**< @brief SSL connection configuration. */
    mbedtls_ssl_context xSslContext;            /**< @brief SSL connection context */
    mbedtls_x509_crt_profile xCertProfile;      /**< @brief Certificate security profile for this connection. */
    mbedtls_x509_crt xRootCaCert;               /**< @brief Root CA certificate context. */
    mbedtls_x509_crt xClientCert;               /**< @brief Client certificate context. */
    mbedtls_pk_context xPrivKey;                /**< @brief Client private key context. */
    mbedtls_entropy_context xEntropyContext;    /**< @brief Entropy context for random number generation. */
    mbedtls_ctr_drbg_context xCtrDrgbContext;   /**< @brief CTR DRBG context for random number generation. */
    const TransportInterfaceExtended_t * pxSocketInterface;
    NetworkContext_t * pxSocketContext;
} TLSContext_t;


/*-----------------------------------------------------------*/

/**
 * @brief Represents string to be logged when mbedTLS returned error
 * does not contain a high-level code.
 */
static const char * pNoHighLevelMbedTlsCodeStr = "<No-High-Level-Code>";

/**
 * @brief Represents string to be logged when mbedTLS returned error
 * does not contain a low-level code.
 */
static const char * pNoLowLevelMbedTlsCodeStr = "<No-Low-Level-Code>";

/**
 * @brief Utility for converting the high-level code in an mbedTLS error to string,
 * if the code-contains a high-level code; otherwise, using a default string.
 */
#define mbedtlsHighLevelCodeOrDefault( mbedTlsCode )        \
    ( mbedtls_high_level_strerr( mbedTlsCode ) != NULL ) ? \
      mbedtls_high_level_strerr( mbedTlsCode ) : pNoHighLevelMbedTlsCodeStr

/**
 * @brief Utility for converting the level-level code in an mbedTLS error to string,
 * if the code-contains a level-level code; otherwise, using a default string.
 */
#define mbedtlsLowLevelCodeOrDefault( mbedTlsCode )        \
    ( mbedtls_low_level_strerr( mbedTlsCode ) != NULL ) ? \
      mbedtls_low_level_strerr( mbedTlsCode ) : pNoLowLevelMbedTlsCodeStr

/*-----------------------------------------------------------*/

/**
 * @brief Initialize the mbed TLS structures in a network connection.
 *
 * @param[in] pxTLSContext The SSL context to initialize.
 */
static void tlsContextInit( TLSContext_t * pxTLSContext );

/**
 * @brief Free the mbed TLS structures in a network connection.
 *
 * @param[in] pxTLSContext The SSL context to free.
 */
static void tlsContextFree( TLSContext_t * pxTLSContext );

/**
 * @brief Add X509 certificate to the trusted list of root certificates.
 *
 * OpenSSL does not provide a single function for reading and loading certificates
 * from files into stores, so the file API must be called. Start with the
 * root certificate.
 *
 * @param[out] pxTLSContext SSL context to which the trusted server root CA is to be added.
 * @param[in] pRootCa PEM-encoded string of the trusted server root CA.
 * @param[in] xRootCaCertSize Size of the trusted server root CA.
 *
 * @return 0 on success; otherwise, failure;
 */
static int32_t setRootCa( TLSContext_t * pxTLSContext,
                          const uint8_t * pRootCa,
                          size_t xRootCaCertSize );

/**
 * @brief Set X509 certificate as client certificate for the server to authenticate.
 *
 * @param[out] pxTLSContext SSL context to which the client certificate is to be set.
 * @param[in] pClientCert PEM-encoded string of the client certificate.
 * @param[in] clientCertSize Size of the client certificate.
 *
 * @return 0 on success; otherwise, failure;
 */
static int32_t setClientCertificate( TLSContext_t * pxTLSContext,
                                     const uint8_t * pClientCert,
                                     size_t clientCertSize );

/**
 * @brief Set private key for the client's certificate.
 *
 * @param[out] pxTLSContext SSL context to which the private key is to be set.
 * @param[in] pPrivateKey PEM-encoded string of the client private key.
 * @param[in] privateKeySize Size of the client private key.
 *
 * @return 0 on success; otherwise, failure;
 */
static int32_t setPrivateKey( TLSContext_t * pxTLSContext,
                              const uint8_t * pPrivateKey,
                              size_t privateKeySize );

/**
 * @brief Passes TLS credentials to the OpenSSL library.
 *
 * Provides the root CA certificate, client certificate, and private key to the
 * OpenSSL library. If the client certificate or private key is not NULL, mutual
 * authentication is used when performing the TLS handshake.
 *
 * @param[out] pxTLSContext SSL context to which the credentials are to be imported.
 * @param[in] pNetworkCredentials TLS credentials to be imported.
 *
 * @return 0 on success; otherwise, failure;
 */
static int32_t setCredentials( TLSContext_t * pxTLSContext,
                               const NetworkCredentials_t * pNetworkCredentials );

/**
 * @brief Set optional configurations for the TLS connection.
 *
 * This function is used to set SNI and ALPN protocols.
 *
 * @param[in] pxTLSContext SSL context to which the optional configurations are to be set.
 * @param[in] pHostName Remote host name, used for server name indication.
 * @param[in] pNetworkCredentials TLS setup parameters.
 */
static void setOptionalConfigurations( TLSContext_t * pxTLSContext,
                                       const char * pHostName,
                                       const NetworkCredentials_t * pNetworkCredentials );

/**
 * @brief Setup TLS by initializing contexts and setting configurations.
 *
 * @param[in] pxNetworkContext Network context.
 * @param[in] pHostName Remote host name, used for server name indication.
 * @param[in] pNetworkCredentials TLS setup parameters.
 *
 * @return #TLS_TRANSPORT_SUCCESS, #TLS_TRANSPORT_INSUFFICIENT_MEMORY, #TLS_TRANSPORT_INVALID_CREDENTIALS,
 * or #TLS_TRANSPORT_INTERNAL_ERROR.
 */
static TlsTransportStatus_t tlsSetup( TLSContext_t * pxTLSContext,
                                      const char * pHostName,
                                      const NetworkCredentials_t * pNetworkCredentials );

/**
 * @brief Perform the TLS handshake on a TCP connection.
 *
 * @param[in] pxNetworkContext Network context.
 * @param[in] pNetworkCredentials TLS setup parameters.
 *
 * @return #TLS_TRANSPORT_SUCCESS, #TLS_TRANSPORT_HANDSHAKE_FAILED, or #TLS_TRANSPORT_INTERNAL_ERROR.
 */
static TlsTransportStatus_t tlsHandshake( TLSContext_t * pxTLSContext,
                                          const NetworkCredentials_t * pNetworkCredentials );

/**
 * @brief Initialize mbedTLS.
 *
 * @param[out] entropyContext mbed TLS entropy context for generation of random numbers.
 * @param[out] ctrDrgbContext mbed TLS CTR DRBG context for generation of random numbers.
 *
 * @return #TLS_TRANSPORT_SUCCESS, or #TLS_TRANSPORT_INTERNAL_ERROR.
 */
static TlsTransportStatus_t initMbedtls( mbedtls_entropy_context * pExntropyContext,
                                         mbedtls_ctr_drbg_context * pxCtrDrgbContext );

//#ifdef MBEDTLS_DEBUG_C
    /* Used to print mbedTLS log output. */
    static void vTLSDebugPrint( void *ctx, int level, const char *file, int line, const char *str );
//#endif

/*-----------------------------------------------------------*/

static void tlsContextInit( TLSContext_t * pxTLSContext )
{
    configASSERT( pxTLSContext != NULL );

    mbedtls_ssl_config_init( &( pxTLSContext->xSslConfig ) );
    mbedtls_x509_crt_init( &( pxTLSContext->xRootCaCert ) );
    mbedtls_pk_init( &( pxTLSContext->xPrivKey ) );
    mbedtls_x509_crt_init( &( pxTLSContext->xClientCert ) );
    mbedtls_ssl_init( &( pxTLSContext->xSslContext ) );

#ifdef MBEDTLS_DEBUG_C
    mbedtls_ssl_conf_dbg( &( pxTLSContext->xSslConfig ), vTLSDebugPrint, NULL );
    mbedtls_debug_set_threshold( MBEDTLS_DEBUG_THRESHOLD );
#endif

    /* Prevent compiler warnings when LogDebug() is defined away. */
    ( void ) pNoLowLevelMbedTlsCodeStr;
    ( void ) pNoHighLevelMbedTlsCodeStr;
}
/*-----------------------------------------------------------*/

static void tlsContextFree( TLSContext_t * pxTLSContext )
{
    configASSERT( pxTLSContext != NULL );

    mbedtls_ssl_free( &( pxTLSContext->xSslContext ) );
    mbedtls_x509_crt_free( &( pxTLSContext->xRootCaCert ) );
    mbedtls_x509_crt_free( &( pxTLSContext->xClientCert ) );
    mbedtls_pk_free( &( pxTLSContext->xPrivKey ) );
    mbedtls_entropy_free( &( pxTLSContext->xEntropyContext ) );
    mbedtls_ctr_drbg_free( &( pxTLSContext->xCtrDrgbContext ) );
    mbedtls_ssl_config_free( &( pxTLSContext->xSslConfig ) );
}
/*-----------------------------------------------------------*/

static int32_t setRootCa( TLSContext_t * pxTLSContext,
                          const uint8_t * pRootCa,
                          size_t xRootCaCertSize )
{
    int32_t mbedtlsError = -1;

    configASSERT( pxTLSContext != NULL );
    configASSERT( pRootCa != NULL );

    /* Parse the server root CA certificate into the SSL context. */
    mbedtlsError = mbedtls_x509_crt_parse( &( pxTLSContext->xRootCaCert ),
                                           pRootCa,
                                           xRootCaCertSize );

    if( mbedtlsError != 0 )
    {
        LogError( "Failed to parse server root CA certificate: mbedTLSError= %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( mbedtlsError ),
                    mbedtlsLowLevelCodeOrDefault( mbedtlsError ) );
    }
    else
    {
        mbedtls_ssl_conf_ca_chain( &( pxTLSContext->xSslConfig ),
                                   &( pxTLSContext->xRootCaCert ),
                                   NULL );
    }

    return mbedtlsError;
}
/*-----------------------------------------------------------*/

static int32_t setClientCertificate( TLSContext_t * pxTLSContext,
                                     const uint8_t * pClientCert,
                                     size_t clientCertSize )
{
    int32_t mbedtlsError = -1;

    configASSERT( pxTLSContext != NULL );
    configASSERT( pClientCert != NULL );

    /* Setup the client certificate. */
    mbedtlsError = mbedtls_x509_crt_parse( &( pxTLSContext->xClientCert ),
                                           pClientCert,
                                           clientCertSize );

    if( mbedtlsError != 0 )
    {
        LogError( "Failed to parse the client certificate: mbedTLSError= %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( mbedtlsError ),
                    mbedtlsLowLevelCodeOrDefault( mbedtlsError ) );
    }

    return mbedtlsError;
}
/*-----------------------------------------------------------*/

static int32_t setPrivateKey( TLSContext_t * pxTLSContext,
                              const uint8_t * pPrivateKeyPath,
                              size_t privateKeySize )
{
    int32_t mbedtlsError = -1;

    configASSERT( pxTLSContext != NULL );
    configASSERT( pPrivateKeyPath != NULL );

    /* Setup the client private key. */
    mbedtlsError = mbedtls_pk_parse_key( &( pxTLSContext->xPrivKey ),
                                         pPrivateKeyPath,
                                         privateKeySize,
                                         NULL,
                                         0 );

    if( mbedtlsError != 0 )
    {
        LogError( "Failed to parse the client key: mbedTLSError= %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( mbedtlsError ),
                    mbedtlsLowLevelCodeOrDefault( mbedtlsError ) );
    }

    return mbedtlsError;
}
/*-----------------------------------------------------------*/

static int32_t setCredentials( TLSContext_t * pxTLSContext,
                               const NetworkCredentials_t * pNetworkCredentials )
{
    int32_t mbedtlsError = -1;

    configASSERT( pxTLSContext != NULL );
    configASSERT( pNetworkCredentials != NULL );

    /* Set up the certificate security profile, starting from the default value. */
    pxTLSContext->xCertProfile = mbedtls_x509_crt_profile_default;

    /* Set SSL authmode and the RNG context. */
//    mbedtls_ssl_conf_authmode( &( pxTLSContext->xSslConfig ),
//                               MBEDTLS_SSL_VERIFY_REQUIRED );

    //TODO FIXME Skipping CA certificate verification
    mbedtls_ssl_conf_authmode( &( pxTLSContext->xSslConfig ),
                                MBEDTLS_SSL_VERIFY_NONE );


    mbedtls_ssl_conf_rng( &( pxTLSContext->xSslConfig ),
                          mbedtls_ctr_drbg_random,
                          &( pxTLSContext->xCtrDrgbContext ) );

    mbedtls_ssl_conf_cert_profile( &( pxTLSContext->xSslConfig ),
                                   &( pxTLSContext->xCertProfile ) );

    mbedtlsError = setRootCa( pxTLSContext,
                              pNetworkCredentials->pRootCa,
                              pNetworkCredentials->rootCaSize );

    if( ( pNetworkCredentials->pClientCert != NULL ) &&
        ( pNetworkCredentials->pPrivateKey != NULL ) )
    {
        if( mbedtlsError == 0 )
        {
            mbedtlsError = setClientCertificate( pxTLSContext,
                                                 pNetworkCredentials->pClientCert,
                                                 pNetworkCredentials->clientCertSize );
        }

        if( mbedtlsError == 0 )
        {
            mbedtlsError = setPrivateKey( pxTLSContext,
                                          pNetworkCredentials->pPrivateKey,
                                          pNetworkCredentials->privateKeySize );
        }

        if( mbedtlsError == 0 )
        {
            mbedtlsError = mbedtls_ssl_conf_own_cert( &( pxTLSContext->xSslConfig ),
                                                      &( pxTLSContext->xClientCert ),
                                                      &( pxTLSContext->xPrivKey ) );
        }
    }

    return mbedtlsError;
}
/*-----------------------------------------------------------*/

static void setOptionalConfigurations( TLSContext_t * pxTLSContext,
                                       const char * pHostName,
                                       const NetworkCredentials_t * pNetworkCredentials )
{
    int32_t mbedtlsError = -1;

    configASSERT( pxTLSContext != NULL );
    configASSERT( pHostName != NULL );
    configASSERT( pNetworkCredentials != NULL );

    if( pNetworkCredentials->pAlpnProtos != NULL )
    {
        /* Include an application protocol list in the TLS ClientHello
         * message. */
        mbedtlsError = mbedtls_ssl_conf_alpn_protocols( &( pxTLSContext->xSslConfig ),
                                                        pNetworkCredentials->pAlpnProtos );

        if( mbedtlsError != 0 )
        {
            LogError( "Failed to configure ALPN protocol in mbed TLS: mbedTLSError= %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( mbedtlsError ),
                        mbedtlsLowLevelCodeOrDefault( mbedtlsError ) );
        }
    }

    /* Enable SNI if requested. */
    if( pNetworkCredentials->disableSni == pdFALSE )
    {
        mbedtlsError = mbedtls_ssl_set_hostname( &( pxTLSContext->xSslContext ),
                                                 pHostName );

        if( mbedtlsError != 0 )
        {
            LogError( "Failed to set server name: mbedTLSError= %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( mbedtlsError ),
                        mbedtlsLowLevelCodeOrDefault( mbedtlsError ) );
        }
    }

    /* Set Maximum Fragment Length if enabled. */
    #ifdef MBEDTLS_SSL_MAX_FRAGMENT_LENGTH

        /* Enable the max fragment extension. 4096 bytes is currently the largest fragment size permitted.
         * See RFC 8449 https://tools.ietf.org/html/rfc8449 for more information.
         *
         * Smaller values can be found in "mbedtls/include/ssl.h".
         */
        mbedtlsError = mbedtls_ssl_conf_max_frag_len( &( pxTLSContext->xSslConfig ), MBEDTLS_SSL_MAX_FRAG_LEN_4096 );

        if( mbedtlsError != 0 )
        {
            LogError( "Failed to maximum fragment length extension: mbedTLSError= %s : %s.",
                      mbedtlsHighLevelCodeOrDefault( mbedtlsError ),
                      mbedtlsLowLevelCodeOrDefault( mbedtlsError ) );
        }
    #endif /* ifdef MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */
}

/*-----------------------------------------------------------*/

static TlsTransportStatus_t tlsSetup( TLSContext_t * pxTLSContext,
                                      const char * pHostName,
                                      const NetworkCredentials_t * pNetworkCredentials )
{
    TlsTransportStatus_t returnStatus = TLS_TRANSPORT_SUCCESS;
    int32_t mbedtlsError = 0;

    configASSERT( pxTLSContext != NULL );
    configASSERT( pHostName != NULL );
    configASSERT( pNetworkCredentials != NULL );
    configASSERT( pNetworkCredentials->pRootCa != NULL );

    /* Initialize the mbed TLS context structures. */
    tlsContextInit( pxTLSContext );

    mbedtlsError = mbedtls_ssl_config_defaults( &( pxTLSContext->xSslConfig ),
                                                MBEDTLS_SSL_IS_CLIENT,
                                                MBEDTLS_SSL_TRANSPORT_STREAM,
                                                MBEDTLS_SSL_PRESET_DEFAULT );

    if( mbedtlsError != 0 )
    {
        LogError( "Failed to set default SSL configuration: mbedTLSError= %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( mbedtlsError ),
                    mbedtlsLowLevelCodeOrDefault( mbedtlsError ) );

        /* Per mbed TLS docs, mbedtls_ssl_config_defaults only fails on memory allocation. */
        returnStatus = TLS_TRANSPORT_INSUFFICIENT_MEMORY;
    }

    if( returnStatus == TLS_TRANSPORT_SUCCESS )
    {
        mbedtlsError = setCredentials( pxTLSContext,
                                       pNetworkCredentials );

        if( mbedtlsError != 0 )
        {
            returnStatus = TLS_TRANSPORT_INVALID_CREDENTIALS;
        }
        else
        {
            /* Optionally set SNI and ALPN protocols. */
            setOptionalConfigurations( pxTLSContext,
                                       pHostName,
                                       pNetworkCredentials );
        }
    }

    return returnStatus;
}

/*-----------------------------------------------------------*/

static int mbedtls_ssl_send( void * pvCtx,
                             const unsigned char * pcBuf,
                             size_t xLen )
{
    TLSContext_t * pxTLSContext = ( TLSContext_t * ) pvCtx;
    const TransportInterfaceExtended_t * pxSock = pxTLSContext->pxSocketInterface;
    int lReturnValue = 0;

    lReturnValue = pxSock->send( pxTLSContext->pxSocketContext,
                                 ( void * const ) pcBuf,
                                 xLen );

    if( lReturnValue < 0 )
    {
        /* force use of newlibc errno */
        switch( *__errno() )
        {
#if EAGAIN != EWOULDBLOCK
        case EAGAIN:
#endif
        case EINTR:
        case EWOULDBLOCK:
            lReturnValue = MBEDTLS_ERR_SSL_WANT_WRITE;
            break;
        case EPIPE:
        case ECONNRESET:
            lReturnValue = MBEDTLS_ERR_NET_CONN_RESET;
            break;
        default:
            lReturnValue = MBEDTLS_ERR_NET_SEND_FAILED;
            break;
        }
    }
    return lReturnValue;
}

/*-----------------------------------------------------------*/

static int mbedtls_ssl_recv( void * pvCtx,
                             unsigned char * pcBuf,
                             size_t xLen )
{
    TLSContext_t * pxTLSContext = ( TLSContext_t * ) pvCtx;
    const TransportInterfaceExtended_t * pxSock = pxTLSContext->pxSocketInterface;
    int lReturnValue = 0;

    lReturnValue = pxSock->recv( pxTLSContext->pxSocketContext,
                                 ( void * ) pcBuf,
                                 xLen );

    if( lReturnValue < 0 )
    {
        /* force use of newlibc errno */
        switch( *__errno() )
        {
#if EAGAIN != EWOULDBLOCK
        case EAGAIN:
#endif
        case EINTR:
        case EWOULDBLOCK:
            lReturnValue = MBEDTLS_ERR_SSL_WANT_READ;
            break;
        case EPIPE:
        case ECONNRESET:
            lReturnValue = MBEDTLS_ERR_NET_CONN_RESET;
            break;
        default:
            lReturnValue = MBEDTLS_ERR_NET_RECV_FAILED;
            break;
        }
    }
    return lReturnValue;
}

/*-----------------------------------------------------------*/

static TlsTransportStatus_t tlsHandshake( TLSContext_t * pxTLSContext,
                                          const NetworkCredentials_t * pNetworkCredentials )
{
    TlsTransportStatus_t returnStatus = TLS_TRANSPORT_SUCCESS;
    int32_t mbedtlsError = 0;

    configASSERT( pxTLSContext != NULL );
    configASSERT( pNetworkCredentials != NULL );

    /* Initialize the mbed TLS secured connection context. */
    mbedtlsError = mbedtls_ssl_setup( &( pxTLSContext->xSslContext ),
                                      &( pxTLSContext->xSslConfig ) );

    if( mbedtlsError != 0 )
    {
        LogError( "Failed to set up mbed TLS SSL context: mbedTLSError= %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( mbedtlsError ),
                    mbedtlsLowLevelCodeOrDefault( mbedtlsError ) );

        returnStatus = TLS_TRANSPORT_INTERNAL_ERROR;
    }
    else
    {
        /* Set the underlying IO for the TLS connection. */

        /* MISRA Rule 11.2 flags the following line for casting the second
         * parameter to void *. This rule is suppressed because
         * #mbedtls_ssl_set_bio requires the second parameter as void *.
         */
        /* coverity[misra_c_2012_rule_11_2_violation] */
        mbedtls_ssl_set_bio( &( pxTLSContext->xSslContext ),
                             ( void * ) pxTLSContext,
                             mbedtls_ssl_send,
                             mbedtls_ssl_recv,
                             NULL );
    }

    if( returnStatus == TLS_TRANSPORT_SUCCESS )
    {
        /* Perform the TLS handshake. */
        do
        {
            mbedtlsError = mbedtls_ssl_handshake( &( pxTLSContext->xSslContext ) );
        }
        while( ( mbedtlsError == MBEDTLS_ERR_SSL_WANT_READ ) ||
               ( mbedtlsError == MBEDTLS_ERR_SSL_WANT_WRITE ) );

        if( mbedtlsError != 0 )
        {
            LogError( "Failed to perform TLS handshake: mbedTLSError= %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( mbedtlsError ),
                        mbedtlsLowLevelCodeOrDefault( mbedtlsError ) );

            returnStatus = TLS_TRANSPORT_HANDSHAKE_FAILED;
        }
        else
        {
            LogInfo( "(Network connection %p) TLS handshake successful.",
                     pxTLSContext );
        }
    }

    return returnStatus;
}
/*-----------------------------------------------------------*/

static TlsTransportStatus_t initMbedtls( mbedtls_entropy_context * pEntropyContext,
                                         mbedtls_ctr_drbg_context * pCtrDrgbContext )
{
    TlsTransportStatus_t returnStatus = TLS_TRANSPORT_SUCCESS;
    int32_t mbedtlsError = 0;

    /* Set the mutex functions for mbed TLS thread safety. */
    mbedtls_threading_set_alt( mbedtls_platform_mutex_init,
                               mbedtls_platform_mutex_free,
                               mbedtls_platform_mutex_lock,
                               mbedtls_platform_mutex_unlock );

    /* Initialize contexts for random number generation. */
    mbedtls_entropy_init( pEntropyContext );
    mbedtls_ctr_drbg_init( pCtrDrgbContext );

    /* Add a strong entropy source. At least one is required. */
    /* Added by STM32-specific rng_alt.c implementation */


    if( mbedtlsError != 0 )
    {
        LogError( "Failed to add entropy source: mbedTLSError= %s : %s.",
                  mbedtlsHighLevelCodeOrDefault( mbedtlsError ),
                  mbedtlsLowLevelCodeOrDefault( mbedtlsError ) );
        returnStatus = TLS_TRANSPORT_INTERNAL_ERROR;
    }

    if( returnStatus == TLS_TRANSPORT_SUCCESS )
    {
        /* Seed the random number generator. */
        mbedtlsError = mbedtls_ctr_drbg_seed( pCtrDrgbContext,
                                              mbedtls_entropy_func,
                                              pEntropyContext,
                                              NULL,
                                              0 );

        if( mbedtlsError != 0 )
        {
            LogError( "Failed to seed PRNG: mbedTLSError= %s : %s.",
                      mbedtlsHighLevelCodeOrDefault( mbedtlsError ),
                      mbedtlsLowLevelCodeOrDefault( mbedtlsError ) );
            returnStatus = TLS_TRANSPORT_INTERNAL_ERROR;
        }
    }

    return returnStatus;
}

/*-----------------------------------------------------------*/

NetworkContext_t * mbedtls_transport_allocate( const TransportInterfaceExtended_t * pxSocketInterface )
{
    TLSContext_t * pxTLSContext;

    if( pxSocketInterface == NULL )
    {
        LogError( "The given pxSocketInterface parameter is NULL." );
    }
    else
    {
        pxTLSContext = ( TLSContext_t * ) pvPortMalloc( sizeof( TLSContext_t ) );
    }


    if( pxTLSContext == NULL )
    {
        LogError( "Could not allocate memory for TLSContext_t." );
    }
    else
    {
        memset( pxTLSContext, 0, sizeof( TLSContext_t ) );
        pxTLSContext->pxSocketInterface = pxSocketInterface;
    }
    return ( NetworkContext_t * ) pxTLSContext;
}

/*-----------------------------------------------------------*/

void mbedtls_transport_free( NetworkContext_t * pxNetworkContext )
{
    vPortFree( pxNetworkContext );
}

/*-----------------------------------------------------------*/

TlsTransportStatus_t mbedtls_transport_connect( NetworkContext_t * pxNetworkContext,
                                           const char * pHostName,
                                           uint16_t port,
                                           const NetworkCredentials_t * pNetworkCredentials,
                                           uint32_t receiveTimeoutMs,
                                           uint32_t sendTimeoutMs )
{
    TlsTransportStatus_t returnStatus = TLS_TRANSPORT_SUCCESS;
    BaseType_t socketError = 0;
    TLSContext_t * pxTLSContext = ( TLSContext_t * ) pxNetworkContext;

    const TransportInterfaceExtended_t * pxSocketInterface = pxTLSContext->pxSocketInterface;

    configASSERT( pxSocketInterface != NULL );

    if( ( pxNetworkContext == NULL ) ||
        ( pHostName == NULL ) ||
        ( pNetworkCredentials == NULL ) )
    {
        LogError( "Invalid input parameter(s): Arguments cannot be NULL. pxNetworkContext=%p, "
                  "pHostName=%p, pNetworkCredentials=%p.",
                  pxNetworkContext,
                  pHostName,
                  pNetworkCredentials );
        returnStatus = TLS_TRANSPORT_INVALID_PARAMETER;
    }
    else if( pNetworkCredentials->pRootCa == NULL)
    {
        LogError( "pRootCa cannot be NULL." );
        returnStatus = TLS_TRANSPORT_INVALID_PARAMETER;
    }
    else
    {
        /* Empty else for MISRA 15.7 compliance. */
    }

    /* Disconnect socket if has previously been connected */
    if( pxTLSContext->pxSocketContext != NULL )
    {
        configASSERT( pxTLSContext->pxSocketInterface != NULL );
        pxTLSContext->pxSocketInterface->close( pxTLSContext->pxSocketContext );
        pxTLSContext->pxSocketContext = NULL;
    }

    /* Allocate a new socket */
    pxTLSContext->pxSocketContext = pxSocketInterface->socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );

    if( pxTLSContext->pxSocketContext == NULL )
    {
        LogError( "Error when allocating socket" );
        returnStatus = TLS_TRANSPORT_INTERNAL_ERROR;
    }

    /* Set send and receive timeout parameters */
    if( returnStatus == TLS_TRANSPORT_SUCCESS )
    {

        socketError |= pxSocketInterface->setsockopt( pxTLSContext->pxSocketContext,
                                                      SO_RCVTIMEO,
                                                      (void *)&receiveTimeoutMs,
                                                      sizeof( receiveTimeoutMs ) );

        socketError |= pxSocketInterface->setsockopt( pxTLSContext->pxSocketContext,
                                                      SO_SNDTIMEO,
                                                      (void *)&sendTimeoutMs,
                                                      sizeof(sendTimeoutMs) );

        if( socketError != SOCK_OK )
        {
            LogError( "Failed to set socket options SO_RCVTIMEO or SO_SNDTIMEO." );
            returnStatus = TLS_TRANSPORT_INVALID_PARAMETER;
        }
    }

    /* Establish a TCP connection with the server. */
    if( returnStatus == TLS_TRANSPORT_SUCCESS )
    {

        socketError = pxSocketInterface->connect_name( pxTLSContext->pxSocketContext,
                                                             pHostName,
                                                             port );
        if( socketError != SOCK_OK )
        {
            LogError( "Failed to connect to %s with error %d.",
                        pHostName,
                        socketError );
            returnStatus = TLS_TRANSPORT_CONNECT_FAILURE;
        }
    }

    /* Initialize mbedtls. */
    if( returnStatus == TLS_TRANSPORT_SUCCESS )
    {
        returnStatus = initMbedtls( &( pxTLSContext->xEntropyContext ),
                                    &( pxTLSContext->xCtrDrgbContext ) );
    }

    /* Initialize TLS contexts and set credentials. */
    if( returnStatus == TLS_TRANSPORT_SUCCESS )
    {
        returnStatus = tlsSetup( pxTLSContext, pHostName, pNetworkCredentials );
    }

    /* Perform TLS handshake. */
    if( returnStatus == TLS_TRANSPORT_SUCCESS )
    {
        returnStatus = tlsHandshake( pxTLSContext, pNetworkCredentials );
    }

    /* Clean up on failure. */
    if( returnStatus != TLS_TRANSPORT_SUCCESS )
    {
        if( pxNetworkContext != NULL )
        {
            tlsContextFree( pxTLSContext );

            if( pxTLSContext->pxSocketContext != NULL )
            {
                /* Call socket close function to deallocate the socket. */
                pxSocketInterface->close( pxTLSContext->pxSocketContext );
                pxTLSContext->pxSocketContext = NULL;
            }
        }
    }
    else
    {
        LogInfo( "(Network connection %p) Connection to %s established.",
                   pxNetworkContext,
                   pHostName );
    }

    return returnStatus;
}
/*-----------------------------------------------------------*/

void mbedtls_transport_disconnect( NetworkContext_t * pxNetworkContext )
{
    BaseType_t tlsStatus = 0;
    TLSContext_t * pxTLSContext = ( TLSContext_t * ) pxNetworkContext;

    if( pxNetworkContext != NULL )
    {
        /* Attempting to terminate TLS connection. */
        tlsStatus = ( BaseType_t ) mbedtls_ssl_close_notify( &( pxTLSContext->xSslContext ) );

        /* Ignore the WANT_READ and WANT_WRITE return values. */
        if( ( tlsStatus != ( BaseType_t ) MBEDTLS_ERR_SSL_WANT_READ ) &&
            ( tlsStatus != ( BaseType_t ) MBEDTLS_ERR_SSL_WANT_WRITE ) )
        {
            if( tlsStatus == 0 )
            {
                LogInfo( "(Network connection %p) TLS close-notify sent.",
                           pxNetworkContext );
            }
            else
            {
                LogError( "(Network connection %p) Failed to send TLS close-notify: mbedTLSError= %s : %s.",
                            pxNetworkContext,
                            mbedtlsHighLevelCodeOrDefault( tlsStatus ),
                            mbedtlsLowLevelCodeOrDefault( tlsStatus ) );
            }
        }
        else
        {
            /* WANT_READ and WANT_WRITE can be ignored. Logging for debugging purposes. */
#ifdef _RB_
            LogInfo( "(Network connection %p) TLS close-notify sent; ",
                       "received %s as the TLS status can be ignored for close-notify."
                       ( tlsStatus == MBEDTLS_ERR_SSL_WANT_READ ) ? "WANT_READ" : "WANT_WRITE",
                       pxNetworkContext );
#endif
        }

        if( pxTLSContext->pxSocketContext != NULL )
        {
            /* Call socket close function to deallocate the socket. */
            pxTLSContext->pxSocketInterface->close( pxTLSContext->pxSocketContext );
            pxTLSContext->pxSocketContext = NULL;
        }

        /* Free mbedTLS contexts. */
        tlsContextFree( pxTLSContext );
    }

    /* Clear the mutex functions for mbed TLS thread safety. */
    mbedtls_threading_free_alt();
}
/*-----------------------------------------------------------*/

int32_t mbedtls_transport_recv( NetworkContext_t * pxNetworkContext,
                                void * pBuffer,
                                size_t bytesToRecv )
{
    int32_t tlsStatus = 0;
    TLSContext_t * pxTLSContext = ( TLSContext_t * ) pxNetworkContext;

    tlsStatus = ( int32_t ) mbedtls_ssl_read( &( pxTLSContext->xSslContext ),
                                              pBuffer,
                                              bytesToRecv );

    if( ( tlsStatus == MBEDTLS_ERR_SSL_TIMEOUT ) ||
        ( tlsStatus == MBEDTLS_ERR_SSL_WANT_READ ) ||
        ( tlsStatus == MBEDTLS_ERR_SSL_WANT_WRITE ) )
    {
        LogDebug( "Failed to read data. However, a read can be retried on this error. "
                    "mbedTLSError= %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( tlsStatus ),
                    mbedtlsLowLevelCodeOrDefault( tlsStatus ) );

        /* Mark these set of errors as a timeout. The libraries may retry read
         * on these errors. */
        tlsStatus = 0;
    }
    else if( tlsStatus < 0 )
    {
        LogError( "Failed to read data: mbedTLSError= %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( tlsStatus ),
                    mbedtlsLowLevelCodeOrDefault( tlsStatus ) );
    }
    else
    {
        /* Empty else marker. */
    }

    return tlsStatus;
}
/*-----------------------------------------------------------*/

int32_t mbedtls_transport_send( NetworkContext_t * pxNetworkContext,
                                const void * pBuffer,
                                size_t bytesToSend )
{
    TLSContext_t * pxTLSContext = ( TLSContext_t * ) pxNetworkContext;
    int32_t tlsStatus = 0;

    tlsStatus = ( int32_t ) mbedtls_ssl_write( &( pxTLSContext->xSslContext ),
                                               pBuffer,
                                               bytesToSend );

    if( ( tlsStatus == MBEDTLS_ERR_SSL_TIMEOUT ) ||
        ( tlsStatus == MBEDTLS_ERR_SSL_WANT_READ ) ||
        ( tlsStatus == MBEDTLS_ERR_SSL_WANT_WRITE ) )
    {
        LogDebug( "Failed to send data. However, send can be retried on this error. "
                    "mbedTLSError= %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( tlsStatus ),
                    mbedtlsLowLevelCodeOrDefault( tlsStatus ) );

        /* Mark these set of errors as a timeout. The libraries may retry send
         * on these errors. */
        tlsStatus = 0;
    }
    else if( tlsStatus < 0 )
    {
        LogError( "Failed to send data:  mbedTLSError= %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( tlsStatus ),
                    mbedtlsLowLevelCodeOrDefault( tlsStatus ) );
    }
    else
    {
        /* Empty else marker. */
    }

    return tlsStatus;
}
/*-----------------------------------------------------------*/



#ifdef MBEDTLS_DEBUG_C

    static inline const char * pcMbedtlsLevelToFrLevel( int lLevel )
    {
        const char * pcFrLogLevel;
        switch( lLevel )
        {
        case 1:
            pcFrLogLevel = "E";
            break;
        case 2:
        case 3:
            pcFrLogLevel = "I";
            break;
        case 4:
        default:
            pcFrLogLevel = "D";
            break;
        }
        return pcFrLogLevel;
    }

    /*-------------------------------------------------------*/

    static void vTLSDebugPrint( void *ctx,
                                int lLevel,
                                const char * pcFileName,
                                int lLineNumber,
                                const char * pcErrStr )
    {
        ( void ) ctx;

        vLoggingPrintf( pcMbedtlsLevelToFrLevel( lLevel ),
                        pcPathToBasename( pcFileName ),
                        lLineNumber,
                        pcErrStr );
    }
#endif


