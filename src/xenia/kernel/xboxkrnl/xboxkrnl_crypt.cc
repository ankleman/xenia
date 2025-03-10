/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2015 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*/

#include <algorithm>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

#ifdef XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"  // for bcrypt.h
#endif

#include "third_party/crypto/TinySHA1.hpp"
#include "third_party/crypto/des/des.cpp"
#include "third_party/crypto/des/des.h"
#include "third_party/crypto/des/des3.h"
#include "third_party/crypto/des/descbc.h"
#include "third_party/crypto/sha256.cpp"
#include "third_party/crypto/sha256.h"

extern "C" {
#include "third_party/aes_128/aes.h"
}

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Value used by x360 for 1024-bit private exponent / 'D' param
uint8_t kStaticPrivateExponent1024[] = {
    0x51, 0xEC, 0x1F, 0x9D, 0x56, 0x26, 0xC2, 0xFC, 0x10, 0xA6, 0x67, 0x64,
    0xCB, 0x3A, 0x6D, 0x4D, 0xA1, 0xE7, 0x4E, 0xA8, 0x42, 0xF0, 0xF4, 0xFD,
    0xFA, 0x66, 0xEF, 0xC7, 0x8E, 0x10, 0x2F, 0xE4, 0x1C, 0xA3, 0x1D, 0xD0,
    0xCE, 0x39, 0x2E, 0xC3, 0x19, 0x2D, 0xD0, 0x58, 0x74, 0x79, 0xAC, 0x08,
    0xE7, 0x90, 0xC1, 0xAC, 0x2D, 0xC6, 0xEB, 0x47, 0xE8, 0x3D, 0xCF, 0x4C,
    0x6D, 0xFF, 0x51, 0x65, 0xD4, 0x6E, 0xBD, 0x0F, 0x15, 0x79, 0x37, 0x95,
    0xC4, 0xAF, 0x90, 0x9E, 0x2B, 0x50, 0x8A, 0x0A, 0x22, 0x4A, 0xB3, 0x41,
    0xE5, 0x89, 0x80, 0x73, 0xCD, 0xFA, 0x21, 0x02, 0xF5, 0xDD, 0x30, 0xDD,
    0x07, 0x2A, 0x6F, 0x34, 0x07, 0x81, 0x97, 0x7E, 0xB2, 0xFB, 0x72, 0xE9,
    0xEA, 0xC1, 0x88, 0x39, 0xAC, 0x48, 0x2B, 0xA8, 0x4D, 0xFC, 0xD7, 0xED,
    0x9B, 0xF9, 0xDE, 0xC2, 0x45, 0x93, 0x4C, 0x4C};

void XeCryptRc4Key(pointer_t<XECRYPT_RC4_STATE> rc4_ctx, lpvoid_t key,
                   dword_t key_size) {
  // Setup RC4 state
  rc4_ctx->i = rc4_ctx->j = 0;
  for (uint32_t x = 0; x < 0x100; x++) {
    rc4_ctx->S[x] = (uint8_t)x;
  }

  uint32_t idx = 0;
  for (uint32_t x = 0; x < 0x100; x++) {
    idx = (idx + rc4_ctx->S[x] + key[x % 0x10]) % 0x100;
    uint8_t temp = rc4_ctx->S[idx];
    rc4_ctx->S[idx] = rc4_ctx->S[x];
    rc4_ctx->S[x] = temp;
  }
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptRc4Key, kNone, kImplemented);

void XeCryptRc4Ecb(pointer_t<XECRYPT_RC4_STATE> rc4_ctx, lpvoid_t data,
                   dword_t size) {
  // Crypt data
  for (uint32_t idx = 0; idx < size; idx++) {
    rc4_ctx->i = (rc4_ctx->i + 1) % 0x100;
    rc4_ctx->j = (rc4_ctx->j + rc4_ctx->S[rc4_ctx->i]) % 0x100;
    uint8_t temp = rc4_ctx->S[rc4_ctx->i];
    rc4_ctx->S[rc4_ctx->i] = rc4_ctx->S[rc4_ctx->j];
    rc4_ctx->S[rc4_ctx->j] = temp;

    uint8_t a = data[idx];
    uint8_t b =
        rc4_ctx->S[(rc4_ctx->S[rc4_ctx->i] + rc4_ctx->S[rc4_ctx->j]) % 0x100];
    data[idx] = (uint8_t)(a ^ b);
  }
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptRc4Ecb, kNone, kImplemented);

void XeCryptRc4(lpvoid_t key, dword_t key_size, lpvoid_t data, dword_t size) {
  XECRYPT_RC4_STATE rc4_ctx;
  XeCryptRc4Key(&rc4_ctx, key, key_size);
  XeCryptRc4Ecb(&rc4_ctx, data, size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptRc4, kNone, kImplemented);

typedef struct {
  xe::be<uint32_t> count;     // 0x0
  xe::be<uint32_t> state[5];  // 0x4
  uint8_t buffer[64];         // 0x18
} XECRYPT_SHA_STATE;
static_assert_size(XECRYPT_SHA_STATE, 0x58);

void InitSha1(sha1::SHA1* sha, const XECRYPT_SHA_STATE* state) {
  uint32_t digest[5];
  std::copy(std::begin(state->state), std::end(state->state), digest);

  sha->init(digest, state->buffer, state->count);
}

void StoreSha1(const sha1::SHA1* sha, XECRYPT_SHA_STATE* state) {
  std::copy_n(sha->getDigest(), xe::countof(state->state), state->state);

  state->count = static_cast<uint32_t>(sha->getByteCount());
  std::copy_n(sha->getBlock(), sha->getBlockByteIndex(), state->buffer);
}

void XeCryptShaInit(pointer_t<XECRYPT_SHA_STATE> sha_state) {
  sha_state.Zero();

  sha_state->state[0] = 0x67452301;
  sha_state->state[1] = 0xEFCDAB89;
  sha_state->state[2] = 0x98BADCFE;
  sha_state->state[3] = 0x10325476;
  sha_state->state[4] = 0xC3D2E1F0;
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptShaInit, kNone, kImplemented);

void XeCryptShaUpdate(pointer_t<XECRYPT_SHA_STATE> sha_state, lpvoid_t input,
                      dword_t input_size) {
  sha1::SHA1 sha;
  InitSha1(&sha, sha_state);

  sha.processBytes(input, input_size);

  StoreSha1(&sha, sha_state);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptShaUpdate, kNone, kImplemented);

void XeCryptShaFinal(pointer_t<XECRYPT_SHA_STATE> sha_state,
                     pointer_t<uint8_t> out, dword_t out_size) {
  sha1::SHA1 sha;
  InitSha1(&sha, sha_state);

  uint8_t digest[0x14];
  sha.finalize(digest);

  std::copy_n(digest, std::min<size_t>(xe::countof(digest), out_size),
              static_cast<uint8_t*>(out));
  std::copy_n(sha.getDigest(), xe::countof(sha_state->state), sha_state->state);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptShaFinal, kNone, kImplemented);

void XeCryptSha(lpvoid_t input_1, dword_t input_1_size, lpvoid_t input_2,
                dword_t input_2_size, lpvoid_t input_3, dword_t input_3_size,
                lpvoid_t output, dword_t output_size) {
  sha1::SHA1 sha;

  if (input_1 && input_1_size) {
    sha.processBytes(input_1, input_1_size);
  }
  if (input_2 && input_2_size) {
    sha.processBytes(input_2, input_2_size);
  }
  if (input_3 && input_3_size) {
    sha.processBytes(input_3, input_3_size);
  }

  uint8_t digest[0x14];
  sha.finalize(digest);
  std::copy_n(digest, std::min<size_t>(xe::countof(digest), output_size),
              output.as<uint8_t*>());
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha, kNone, kImplemented);

// TODO: Size of this struct hasn't been confirmed yet.
typedef struct {
  xe::be<uint32_t> count;     // 0x0
  xe::be<uint32_t> state[8];  // 0x4
  uint8_t buffer[64];         // 0x24
} XECRYPT_SHA256_STATE;

void XeCryptSha256Init(pointer_t<XECRYPT_SHA256_STATE> sha_state) {
  sha_state.Zero();

  sha_state->state[0] = 0x6a09e667;
  sha_state->state[1] = 0xbb67ae85;
  sha_state->state[2] = 0x3c6ef372;
  sha_state->state[3] = 0xa54ff53a;
  sha_state->state[4] = 0x510e527f;
  sha_state->state[5] = 0x9b05688c;
  sha_state->state[6] = 0x1f83d9ab;
  sha_state->state[7] = 0x5be0cd19;
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha256Init, kNone, kImplemented);

void XeCryptSha256Update(pointer_t<XECRYPT_SHA256_STATE> sha_state,
                         lpvoid_t input, dword_t input_size) {
  sha256::SHA256 sha;
  std::copy(std::begin(sha_state->state), std::end(sha_state->state),
            sha.getHashValues());
  std::copy(std::begin(sha_state->buffer), std::end(sha_state->buffer),
            sha.getBuffer());
  sha.setTotalSize(sha_state->count);

  sha.add(input, input_size);

  std::copy_n(sha.getHashValues(), xe::countof(sha_state->state),
              sha_state->state);
  std::copy_n(sha.getBuffer(), xe::countof(sha_state->buffer),
              sha_state->buffer);
  sha_state->count = static_cast<uint32_t>(sha.getTotalSize());
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha256Update, kNone, kImplemented);

void XeCryptSha256Final(pointer_t<XECRYPT_SHA256_STATE> sha_state,
                        pointer_t<uint8_t> out, dword_t out_size) {
  sha256::SHA256 sha;
  std::copy(std::begin(sha_state->state), std::end(sha_state->state),
            sha.getHashValues());
  std::copy(std::begin(sha_state->buffer), std::end(sha_state->buffer),
            sha.getBuffer());
  sha.setTotalSize(sha_state->count);

  uint8_t hash[32];
  sha.getHash(hash);

  std::copy_n(hash, std::min<size_t>(xe::countof(hash), out_size),
              static_cast<uint8_t*>(out));
  std::copy(std::begin(hash), std::end(hash), sha_state->buffer);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha256Final, kNone, kImplemented);

// Byteswaps each 8 bytes
void XeCryptBnQw_SwapDwQwLeBe(pointer_t<uint64_t> qw_inp,
                              pointer_t<uint64_t> qw_out, dword_t size) {
  xe::copy_and_swap<uint64_t>(qw_out, qw_inp, size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptBnQw_SwapDwQwLeBe, kNone, kImplemented);

dword_result_t XeCryptBnQwNeRsaPrvCrypt(pointer_t<uint64_t> qw_a,
                                        pointer_t<uint64_t> qw_b,
                                        pointer_t<XECRYPT_RSA> rsa) {
  // 0 indicates failure (but not a BOOL return value)
#ifndef XE_PLATFORM_WIN32
  XELOGE(
      "XeCryptBnQwNeRsaPrvCrypt called but no implementation available for "
      "this platform!");
  assert_always();
  return 1;
#else
  uint32_t key_digits = rsa->size;

  if (key_digits <= 0 || key_digits > 0x40) {
    return false;  // too large
  }

  // TODO: we only have PrivExp/'D' value for 1024-bit keys right now
  // It should be possible to calculate it though, if we had some support for
  // big numbers
  if (key_digits != 0x10) {
    return false;
  }

  uint32_t modulus_size = key_digits * 8;
  uint32_t prime_count = key_digits / 2;
  uint32_t prime_size = prime_count * 8;

  // Convert XECRYPT blob into BCrypt format
  ULONG key_size = sizeof(BCRYPT_RSAKEY_BLOB) + sizeof(uint32_t) +  // exponent
                   modulus_size +                                   // modulus
                   prime_size +                                     // prime1
                   prime_size +                                     // prime2
                   prime_size +                                     // exponent1
                   prime_size +                                     // exponent2
                   prime_size +   // coefficient
                   modulus_size;  // private exponent / 'D'
  auto key_buf = std::make_unique<uint8_t[]>(key_size);
  auto* key_header = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(key_buf.get());

  key_header->Magic = BCRYPT_RSAFULLPRIVATE_MAGIC;
  key_header->BitLength = modulus_size * 8;
  key_header->cbPublicExp = sizeof(uint32_t);
  key_header->cbModulus = modulus_size;
  key_header->cbPrime1 = key_header->cbPrime2 = prime_size;

  // Copy in exponent/modulus, luckily these are BE inside BCrypt blob
  uint32_t* key_exponent = reinterpret_cast<uint32_t*>(&key_header[1]);
  *key_exponent = rsa->public_exponent.value;

  // ...except modulus needs to be reversed in 64-bit chunks for BCrypt to make
  // use of it properly for some reason
  uint64_t* key_modulus = reinterpret_cast<uint64_t*>(&key_exponent[1]);
  const uint64_t* xecrypt_modulus = reinterpret_cast<const uint64_t*>(&rsa[1]);
  std::reverse_copy(xecrypt_modulus, xecrypt_modulus + key_digits, key_modulus);

  uint64_t* key_prime1 = reinterpret_cast<uint64_t*>(&key_modulus[key_digits]);
  const uint64_t* xecrypt_prime1 =
      reinterpret_cast<const uint64_t*>(&xecrypt_modulus[key_digits]);

  std::reverse_copy(xecrypt_prime1, xecrypt_prime1 + (prime_count), key_prime1);

  uint64_t* key_prime2 = reinterpret_cast<uint64_t*>(&key_prime1[prime_count]);
  const uint64_t* xecrypt_prime2 =
      reinterpret_cast<const uint64_t*>(&xecrypt_prime1[prime_count]);

  std::reverse_copy(xecrypt_prime2, xecrypt_prime2 + prime_count, key_prime2);

  uint64_t* key_exponent1 =
      reinterpret_cast<uint64_t*>(&key_prime2[prime_count]);
  const uint64_t* xecrypt_exponent1 =
      reinterpret_cast<const uint64_t*>(&xecrypt_prime2[prime_count]);

  std::reverse_copy(xecrypt_exponent1, xecrypt_exponent1 + prime_count,
                    key_exponent1);

  uint64_t* key_exponent2 =
      reinterpret_cast<uint64_t*>(&key_exponent1[prime_count]);
  const uint64_t* xecrypt_exponent2 =
      reinterpret_cast<const uint64_t*>(&xecrypt_exponent1[prime_count]);

  std::reverse_copy(xecrypt_exponent2, xecrypt_exponent2 + prime_count,
                    key_exponent2);

  uint64_t* key_coefficient =
      reinterpret_cast<uint64_t*>(&key_exponent2[prime_count]);
  const uint64_t* xecrypt_coefficient =
      reinterpret_cast<const uint64_t*>(&xecrypt_exponent2[prime_count]);

  std::reverse_copy(xecrypt_coefficient, xecrypt_coefficient + prime_count,
                    key_coefficient);

  uint64_t* key_privexponent =
      reinterpret_cast<uint64_t*>(&key_coefficient[prime_count]);

  // X360 uses a static private exponent / "D" value
  std::memcpy(key_privexponent, kStaticPrivateExponent1024, 0x80);

  BCRYPT_ALG_HANDLE hAlgorithm = NULL;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &hAlgorithm, BCRYPT_RSA_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);

  if (!BCRYPT_SUCCESS(status)) {
    XELOGE(
        "XeCryptBnQwNeRsaPrvCrypt: BCryptOpenAlgorithmProvider failed with "
        "status {:#X}!",
        status);
    return 0;
  }

  BCRYPT_KEY_HANDLE hKey = NULL;
  status = BCryptImportKeyPair(hAlgorithm, NULL, BCRYPT_RSAFULLPRIVATE_BLOB,
                               &hKey, key_buf.get(), key_size, 0);

  if (!BCRYPT_SUCCESS(status)) {
    XELOGE(
        "XeCryptBnQwNeRsaPrvCrypt: BCryptImportKeyPair failed with status "
        "{:#X}!",
        status);

    if (hAlgorithm) {
      BCryptCloseAlgorithmProvider(hAlgorithm, 0);
    }

    return 0;
  }

  // Byteswap & reverse the input into output, as BCrypt wants MSB first
  uint64_t* output = qw_b;
  uint8_t* output_bytes = reinterpret_cast<uint8_t*>(output);
  xe::copy_and_swap<uint64_t>(output, qw_a, key_digits);
  std::reverse(output_bytes, output_bytes + modulus_size);

  // BCryptDecrypt only works with private keys, fortunately BCryptEncrypt
  // performs the right actions needed for us to decrypt the input
  ULONG result_size = 0;
  status =
      BCryptDecrypt(hKey, output_bytes, modulus_size, nullptr, nullptr, 0,
                    output_bytes, modulus_size, &result_size, BCRYPT_PAD_NONE);

  assert(result_size == modulus_size);

  if (!BCRYPT_SUCCESS(status)) {
    XELOGE("XeCryptBnQwNeRsaPrvCrypt: BCryptDecrypt failed with status {:#X}!",
           status);
  } else {
    // Reverse data & byteswap again so data is as game expects
    std::reverse(output_bytes, output_bytes + modulus_size);
    xe::copy_and_swap(output, output, key_digits);
  }

  if (hKey) {
    BCryptDestroyKey(hKey);
  }
  if (hAlgorithm) {
    BCryptCloseAlgorithmProvider(hAlgorithm, 0);
  }

  return BCRYPT_SUCCESS(status) ? 1 : 0;
#endif
}
#ifdef XE_PLATFORM_WIN32
DECLARE_XBOXKRNL_EXPORT1(XeCryptBnQwNeRsaPrvCrypt, kNone, kImplemented);
#else
DECLARE_XBOXKRNL_EXPORT1(XeCryptBnQwNeRsaPrvCrypt, kNone, kStub);
#endif

dword_result_t XeCryptBnQwNeRsaPubCrypt(pointer_t<uint64_t> qw_a,
                                        pointer_t<uint64_t> qw_b,
                                        pointer_t<XECRYPT_RSA> rsa) {
  // 0 indicates failure (but not a BOOL return value)
#ifndef XE_PLATFORM_WIN32
  XELOGE(
      "XeCryptBnQwNeRsaPubCrypt called but no implementation available for "
      "this platform!");
  assert_always();
  return 1;
#else
  uint32_t key_digits = rsa->size;
  uint32_t modulus_size = key_digits * 8;

  // Convert XECRYPT blob into BCrypt format
  ULONG key_size = sizeof(BCRYPT_RSAKEY_BLOB) + sizeof(uint32_t) + modulus_size;
  auto key_buf = std::make_unique<uint8_t[]>(key_size);
  auto* key_header = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(key_buf.get());

  key_header->Magic = BCRYPT_RSAPUBLIC_MAGIC;
  key_header->BitLength = modulus_size * 8;
  key_header->cbPublicExp = sizeof(uint32_t);
  key_header->cbModulus = modulus_size;
  key_header->cbPrime1 = key_header->cbPrime2 = 0;

  // Copy in exponent/modulus, luckily these are BE inside BCrypt blob
  uint32_t* key_exponent = reinterpret_cast<uint32_t*>(&key_header[1]);
  *key_exponent = rsa->public_exponent.value;

  // ...except modulus needs to be reversed in 64-bit chunks for BCrypt to make
  // use of it properly for some reason
  uint64_t* key_modulus = reinterpret_cast<uint64_t*>(&key_exponent[1]);
  uint64_t* xecrypt_modulus = reinterpret_cast<uint64_t*>(&rsa[1]);
  std::reverse_copy(xecrypt_modulus, xecrypt_modulus + key_digits, key_modulus);

  BCRYPT_ALG_HANDLE hAlgorithm = NULL;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &hAlgorithm, BCRYPT_RSA_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);

  if (!BCRYPT_SUCCESS(status)) {
    XELOGE(
        "XeCryptBnQwNeRsaPubCrypt: BCryptOpenAlgorithmProvider failed with "
        "status {:#X}!",
        status);
    return 0;
  }

  BCRYPT_KEY_HANDLE hKey = NULL;
  status = BCryptImportKeyPair(hAlgorithm, NULL, BCRYPT_RSAPUBLIC_BLOB, &hKey,
                               key_buf.get(), key_size, 0);

  if (!BCRYPT_SUCCESS(status)) {
    XELOGE(
        "XeCryptBnQwNeRsaPubCrypt: BCryptImportKeyPair failed with status "
        "{:#X}!",
        status);

    if (hAlgorithm) {
      BCryptCloseAlgorithmProvider(hAlgorithm, 0);
    }

    return 0;
  }

  // Byteswap & reverse the input into output, as BCrypt wants MSB first
  uint64_t* output = qw_b;
  uint8_t* output_bytes = reinterpret_cast<uint8_t*>(output);
  xe::copy_and_swap<uint64_t>(output, qw_a, key_digits);
  std::reverse(output_bytes, output_bytes + modulus_size);

  // BCryptDecrypt only works with private keys, fortunately BCryptEncrypt
  // performs the right actions needed for us to decrypt the input
  ULONG result_size = 0;
  status =
      BCryptEncrypt(hKey, output_bytes, modulus_size, nullptr, nullptr, 0,
                    output_bytes, modulus_size, &result_size, BCRYPT_PAD_NONE);

  assert(result_size == modulus_size);

  if (!BCRYPT_SUCCESS(status)) {
    XELOGE("XeCryptBnQwNeRsaPubCrypt: BCryptEncrypt failed with status {:#X}!",
           status);
  } else {
    // Reverse data & byteswap again so data is as game expects
    std::reverse(output_bytes, output_bytes + modulus_size);
    xe::copy_and_swap(output, output, key_digits);
  }

  if (hKey) {
    BCryptDestroyKey(hKey);
  }
  if (hAlgorithm) {
    BCryptCloseAlgorithmProvider(hAlgorithm, 0);
  }

  return BCRYPT_SUCCESS(status) ? 1 : 0;
#endif
}
#ifdef XE_PLATFORM_WIN32
DECLARE_XBOXKRNL_EXPORT1(XeCryptBnQwNeRsaPubCrypt, kNone, kImplemented);
#else
DECLARE_XBOXKRNL_EXPORT1(XeCryptBnQwNeRsaPubCrypt, kNone, kStub);
#endif

uint64_t kPkcs1Format0_0 = 0xE03021A05000414;
uint64_t kPkcs1Format0_1 = 0x3021300906052B;

uint64_t kPkcs1Format1_0 = 0x052B0E03021A0414;
uint32_t kPkcs1Format1_1 = 0x1F300706;
uint16_t kPkcs1Format1_2 = 0x30;

void XeCryptBnDwLePkcs1Format(lpvoid_t hash, dword_t format,
                              lpvoid_t output_sig, dword_t output_sig_size) {
  std::memset(output_sig, 0xFF, output_sig_size);

  if (output_sig_size - 39 > 473) return;

  output_sig[output_sig_size - 1] = 0;
  output_sig[output_sig_size - 2] = 1;

  auto* hash_ptr = reinterpret_cast<uint8_t*>(hash.host_address());

  // Copy reversed-hash into signature
  std::reverse_copy(hash_ptr, hash_ptr + 0x14,
                    reinterpret_cast<uint8_t*>(output_sig.host_address()));

  // Append different bytes depending on format
  switch (format) {
    case 0:
      *(uint64_t*)(output_sig + 0x14) = kPkcs1Format0_0;
      *(uint64_t*)(output_sig + 0x1C) = kPkcs1Format0_1;
      break;
    case 1:
      *(uint64_t*)(output_sig + 0x14) = kPkcs1Format1_0;
      *(uint32_t*)(output_sig + 0x1C) = kPkcs1Format1_1;
      *(uint16_t*)(output_sig + 0x20) = kPkcs1Format1_2;
      break;
    case 2:
      output_sig[0x14] = 0;
  }
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptBnDwLePkcs1Format, kNone, kStub);

dword_result_t XeCryptBnDwLePkcs1Verify(lpvoid_t hash, lpvoid_t input_sig,
                                        dword_t input_sig_size) {
  // returns BOOL

  if (input_sig_size - 39 > 473) {
    return false;
  }

  // format = 0 if 0x16 == 0
  // format = 1 if 0x16 == 0x1A
  // format = 2 if 0x16 != 0x1A
  uint32_t format = 0;
  if (input_sig[0x16] != 0) {
    format = (input_sig[0x16] != 0x1A) ? 2 : 1;
  }

  auto test_sig = std::make_unique<uint8_t[]>(input_sig_size);
  XeCryptBnDwLePkcs1Format(hash, format, test_sig.get(), input_sig_size);

  return std::memcmp(test_sig.get(), input_sig, input_sig_size) == 0;
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptBnDwLePkcs1Verify, kNone, kStub);

void XeCryptRandom(lpvoid_t buf, dword_t buf_size) {
  std::memset(buf, 0xFD, buf_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptRandom, kNone, kStub);

struct XECRYPT_DES_STATE {
  uint32_t keytab[16][2];
};

// Sets bit 0 to make the parity odd
void XeCryptDesParity(lpvoid_t inp, dword_t inp_size, lpvoid_t out_ptr) {
  DES::set_parity(inp, inp_size, out_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptDesParity, kNone, kImplemented);

struct XECRYPT_DES3_STATE {
  XECRYPT_DES_STATE des_state[3];
};

void XeCryptDes3Key(pointer_t<XECRYPT_DES3_STATE> state_ptr, lpqword_t key) {
  DES3 des3(key[0], key[1], key[2]);
  DES* des = des3.getDES();

  // Store our DES state into the state.
  for (int i = 0; i < 3; i++) {
    std::memcpy(state_ptr->des_state[i].keytab, des[i].get_sub_key(), 128);
  }
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptDes3Key, kNone, kImplemented);

void XeCryptDes3Ecb(pointer_t<XECRYPT_DES3_STATE> state_ptr, lpqword_t inp,
                    lpqword_t out, dword_t encrypt) {
  DES3 des3((ui64*)state_ptr->des_state[0].keytab,
            (ui64*)state_ptr->des_state[1].keytab,
            (ui64*)state_ptr->des_state[2].keytab);

  if (encrypt) {
    *out = des3.encrypt(*inp);
  } else {
    *out = des3.decrypt(*inp);
  }
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptDes3Ecb, kNone, kImplemented);

void XeCryptDes3Cbc(pointer_t<XECRYPT_DES3_STATE> state_ptr, lpqword_t inp,
                    dword_t inp_size, lpqword_t out, lpqword_t feed,
                    dword_t encrypt) {
  DES3 des3((ui64*)state_ptr->des_state[0].keytab,
            (ui64*)state_ptr->des_state[1].keytab,
            (ui64*)state_ptr->des_state[2].keytab);

  // DES can only do 8-byte chunks at a time!
  assert_true(inp_size % 8 == 0);

  uint64_t last_block = *feed;
  for (uint32_t i = 0; i < inp_size / 8; i++) {
    uint64_t block = inp[i];
    if (encrypt) {
      last_block = des3.encrypt(block ^ last_block);
      out[i] = last_block;
    } else {
      out[i] = des3.decrypt(block) ^ last_block;
      last_block = block;
    }
  }

  *feed = last_block;
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptDes3Cbc, kNone, kImplemented);

static inline uint8_t xeXeCryptAesMul2(uint8_t a) {
  return (a & 0x80) ? ((a << 1) ^ 0x1B) : (a << 1);
}

void XeCryptAesKey(pointer_t<XECRYPT_AES_STATE> state_ptr, lpvoid_t key) {
  aes_key_schedule_128(key, reinterpret_cast<uint8_t*>(state_ptr->keytabenc));
  // Decryption key schedule not needed by openluopworld/aes_128, but generated
  // to fill the context structure properly.
  std::memcpy(state_ptr->keytabdec[0], state_ptr->keytabenc[10], 16);
  // Inverse MixColumns.
  for (uint32_t i = 1; i < 10; ++i) {
    const uint8_t* enc =
        reinterpret_cast<const uint8_t*>(state_ptr->keytabenc[10 - i]);
    uint8_t* dec = reinterpret_cast<uint8_t*>(state_ptr->keytabdec[i]);
    uint8_t t, u, v;
    t = enc[0] ^ enc[1] ^ enc[2] ^ enc[3];
    dec[0] = t ^ enc[0] ^ xeXeCryptAesMul2(enc[0] ^ enc[1]);
    dec[1] = t ^ enc[1] ^ xeXeCryptAesMul2(enc[1] ^ enc[2]);
    dec[2] = t ^ enc[2] ^ xeXeCryptAesMul2(enc[2] ^ enc[3]);
    dec[3] = t ^ enc[3] ^ xeXeCryptAesMul2(enc[3] ^ enc[0]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[0] ^ enc[2]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[1] ^ enc[3]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[0] ^= t ^ u;
    dec[1] ^= t ^ v;
    dec[2] ^= t ^ u;
    dec[3] ^= t ^ v;
    t = enc[4] ^ enc[5] ^ enc[6] ^ enc[7];
    dec[4] = t ^ enc[4] ^ xeXeCryptAesMul2(enc[4] ^ enc[5]);
    dec[5] = t ^ enc[5] ^ xeXeCryptAesMul2(enc[5] ^ enc[6]);
    dec[6] = t ^ enc[6] ^ xeXeCryptAesMul2(enc[6] ^ enc[7]);
    dec[7] = t ^ enc[7] ^ xeXeCryptAesMul2(enc[7] ^ enc[4]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[4] ^ enc[6]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[5] ^ enc[7]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[4] ^= t ^ u;
    dec[5] ^= t ^ v;
    dec[6] ^= t ^ u;
    dec[7] ^= t ^ v;
    t = enc[8] ^ enc[9] ^ enc[10] ^ enc[11];
    dec[8] = t ^ enc[8] ^ xeXeCryptAesMul2(enc[8] ^ enc[9]);
    dec[9] = t ^ enc[9] ^ xeXeCryptAesMul2(enc[9] ^ enc[10]);
    dec[10] = t ^ enc[10] ^ xeXeCryptAesMul2(enc[10] ^ enc[11]);
    dec[11] = t ^ enc[11] ^ xeXeCryptAesMul2(enc[11] ^ enc[8]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[8] ^ enc[10]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[9] ^ enc[11]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[8] ^= t ^ u;
    dec[9] ^= t ^ v;
    dec[10] ^= t ^ u;
    dec[11] ^= t ^ v;
    t = enc[12] ^ enc[13] ^ enc[14] ^ enc[15];
    dec[12] = t ^ enc[12] ^ xeXeCryptAesMul2(enc[12] ^ enc[13]);
    dec[13] = t ^ enc[13] ^ xeXeCryptAesMul2(enc[13] ^ enc[14]);
    dec[14] = t ^ enc[14] ^ xeXeCryptAesMul2(enc[14] ^ enc[15]);
    dec[15] = t ^ enc[15] ^ xeXeCryptAesMul2(enc[15] ^ enc[12]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[12] ^ enc[14]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[13] ^ enc[15]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[12] ^= t ^ u;
    dec[13] ^= t ^ v;
    dec[14] ^= t ^ u;
    dec[15] ^= t ^ v;
  }
  std::memcpy(state_ptr->keytabdec[10], state_ptr->keytabenc[0], 16);
  // TODO(Triang3l): Verify the order in keytabenc and everything in keytabdec.
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptAesKey, kNone, kImplemented);

void XeCryptAesEcb(pointer_t<XECRYPT_AES_STATE> state_ptr, lpvoid_t inp_ptr,
                   lpvoid_t out_ptr, dword_t encrypt) {
  const uint8_t* keytab =
      reinterpret_cast<const uint8_t*>(state_ptr->keytabenc);
  if (encrypt) {
    aes_encrypt_128(keytab, inp_ptr, out_ptr);
  } else {
    aes_decrypt_128(keytab, inp_ptr, out_ptr);
  }
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptAesEcb, kNone, kImplemented);

void XeCryptAesCbc(pointer_t<XECRYPT_AES_STATE> state_ptr, lpvoid_t inp_ptr,
                   dword_t inp_size, lpvoid_t out_ptr, lpvoid_t feed_ptr,
                   dword_t encrypt) {
  const uint8_t* keytab =
      reinterpret_cast<const uint8_t*>(state_ptr->keytabenc);
  const uint8_t* inp = inp_ptr.as<const uint8_t*>();
  uint8_t* out = out_ptr.as<uint8_t*>();
  uint8_t* feed = feed_ptr.as<uint8_t*>();
  if (encrypt) {
    for (uint32_t i = 0; i < inp_size; i += 16) {
      for (uint32_t j = 0; j < 16; ++j) {
        feed[j] ^= inp[j];
      }
      aes_encrypt_128(keytab, feed, feed);
      std::memcpy(out, feed, 16);
      inp += 16;
      out += 16;
    }
  } else {
    for (uint32_t i = 0; i < inp_size; i += 16) {
      // In case inp == out.
      uint8_t tmp[16];
      std::memcpy(tmp, inp, 16);
      aes_decrypt_128(keytab, inp, out);
      for (uint32_t j = 0; j < 16; ++j) {
        out[j] ^= feed[j];
      }
      std::memcpy(feed, tmp, 16);
      inp += 16;
      out += 16;
    }
  }
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptAesCbc, kNone, kImplemented);

void XeCryptHmacSha(lpvoid_t key, dword_t key_size_in, lpvoid_t inp_1,
                    dword_t inp_1_size, lpvoid_t inp_2, dword_t inp_2_size,
                    lpvoid_t inp_3, dword_t inp_3_size, lpvoid_t out,
                    dword_t out_size) {
  uint32_t key_size = key_size_in;
  sha1::SHA1 sha;
  uint8_t kpad_i[0x40];
  uint8_t kpad_o[0x40];
  uint8_t tmp_key[0x40];
  std::memset(kpad_i, 0x36, 0x40);
  std::memset(kpad_o, 0x5C, 0x40);

  // Setup HMAC key
  // If > block size, use its hash
  if (key_size > 0x40) {
    sha1::SHA1 sha_key;
    sha_key.processBytes(key, key_size);
    sha_key.finalize((uint8_t*)tmp_key);

    key_size = 0x14u;
  } else {
    std::memcpy(tmp_key, key, key_size);
  }

  for (uint32_t i = 0; i < key_size; i++) {
    kpad_i[i] = tmp_key[i] ^ 0x36;
    kpad_o[i] = tmp_key[i] ^ 0x5C;
  }

  // Inner
  sha.processBytes(kpad_i, 0x40);

  if (inp_1_size) {
    sha.processBytes(inp_1, inp_1_size);
  }

  if (inp_2_size) {
    sha.processBytes(inp_2, inp_2_size);
  }

  if (inp_3_size) {
    sha.processBytes(inp_3, inp_3_size);
  }

  uint8_t digest[0x14];
  sha.finalize(digest);
  sha.reset();

  // Outer
  sha.processBytes(kpad_o, 0x40);
  sha.processBytes(digest, 0x14);
  sha.finalize(digest);

  std::memcpy(out, digest, std::min((uint32_t)out_size, 0x14u));
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptHmacSha, kNone, kImplemented);

void RegisterCryptExports(xe::cpu::ExportResolver* export_resolver,
                          KernelState* kernel_state) {}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
