#include <stdio.h>
#include <string.h>
#include "mbedtls/sha256.h"
int main() {
  unsigned char out[32]; char hex[65];
  const char* v[][2] = {
    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}};
  for (int i = 0; i < 3; i++) {
    mbedtls_sha256((const unsigned char*)v[i][0], strlen(v[i][0]), out, 0);
    for (int j = 0; j < 32; j++) sprintf(hex + 2*j, "%02x", out[j]);
    if (strcmp(hex, v[i][1])) { printf("FAIL %d: %s\n", i, hex); return 1; }
  }
  printf("SHA256_VECTORS_OK\n");
  return 0;
}
