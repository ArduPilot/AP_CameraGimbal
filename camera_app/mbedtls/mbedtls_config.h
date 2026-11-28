/* Mbed TLS 4.3 configuration for the cloud uploader: a TLS 1.2 / 1.3 client
 * only, with certificate verification against a CA file. Based on Mbed TLS's
 * own small Suite-B example; see camera_app/mbedtls/crypto_config.h for the
 * algorithms. */
#ifndef CA_MBEDTLS_CONFIG_H
#define CA_MBEDTLS_CONFIG_H

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE /* TLS 1.3 needs it */

#define MBEDTLS_ERROR_C /* readable TLS errors for the upload status */
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C

/* Google's responses are small; uploads are sent in chunks. */
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#endif
