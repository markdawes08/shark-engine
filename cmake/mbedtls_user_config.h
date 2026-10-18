// Private Mbed TLS build configuration. The transport also enforces TLS 1.3,
// its negotiated algorithms, peer authentication and bounded connection queues.
#ifndef SHARK_MBEDTLS_USER_CONFIG_H
#define SHARK_MBEDTLS_USER_CONFIG_H

#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 16384
#define MBEDTLS_X509_MAX_INTERMEDIATE_CA 4

#endif
