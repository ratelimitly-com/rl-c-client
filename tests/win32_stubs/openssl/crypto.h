#ifndef TEST_OPENSSL_CRYPTO_H
#define TEST_OPENSSL_CRYPTO_H

#include <stddef.h>

void OPENSSL_cleanse(void *pointer, size_t length);
int CRYPTO_memcmp(const void *left, const void *right, size_t length);

#endif
