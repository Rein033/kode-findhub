/* kode-findhub: micro-ecc renamed, so it can link next to ESP-IDF's tinycrypt (bt), which
 * exports the same uECC_* names. Included first by uECC.h. */
#ifndef KFIND_UECC_PREFIX_H_
#define KFIND_UECC_PREFIX_H_
#define uECC_compute_public_key kfind_uECC_compute_public_key
#define uECC_curve_private_key_size kfind_uECC_curve_private_key_size
#define uECC_curve_public_key_size kfind_uECC_curve_public_key_size
#define uECC_get_rng kfind_uECC_get_rng
#define uECC_make_key kfind_uECC_make_key
#define uECC_secp160r1 kfind_uECC_secp160r1
#define uECC_set_rng kfind_uECC_set_rng
#define uECC_shared_secret kfind_uECC_shared_secret
#define uECC_sign kfind_uECC_sign
#define uECC_sign_deterministic kfind_uECC_sign_deterministic
#define uECC_sign_with_k kfind_uECC_sign_with_k
#define uECC_valid_public_key kfind_uECC_valid_public_key
#define uECC_verify kfind_uECC_verify
#endif
