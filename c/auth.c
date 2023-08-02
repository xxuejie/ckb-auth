// clang-format off
#include "mbedtls/md.h"
#include "mbedtls/md_internal.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "ed25519.h"

// configuration for secp256k1
#define ENABLE_MODULE_EXTRAKEYS
#define ENABLE_MODULE_SCHNORRSIG
#define SECP256K1_BUILD
#define SECP256K1_API
// in secp256k1_ctz64_var: we don't have __builtin_ctzl in gcc for RISC-V
#define __builtin_ctzl secp256k1_ctz64_var_debruijn

#include "ckb_consts.h"
#if defined(CKB_USE_SIM)
// exclude ckb_dlfcn.h
#define CKB_C_STDLIB_CKB_DLFCN_H_
#include "ckb_syscall_auth_sim.h"
#else
#include "ckb_syscalls.h"
#endif

#include "ckb_keccak256.h"
#include "secp256k1_helper_20210801.h"
#include "include/secp256k1_schnorrsig.h"

#include "ckb_auth.h"
#undef CKB_SUCCESS
#include "ckb_hex.h"
#include "blake2b.h"
// clang-format on

#include "cardano/cardano_lock_inc.h"

// secp256k1 also defines this macros
#undef CHECK2
#undef CHECK
#define CHECK2(cond, code) \
    do {                   \
        if (!(cond)) {     \
            err = code;    \
            goto exit;     \
        }                  \
    } while (0)

#define CHECK(code)      \
    do {                 \
        if (code != 0) { \
            err = code;  \
            goto exit;   \
        }                \
    } while (0)

#define CKB_AUTH_LEN 21
#define BLAKE160_SIZE 20
#define BLAKE2B_BLOCK_SIZE 32
#define SECP256K1_PUBKEY_SIZE 33
#define UNCOMPRESSED_SECP256K1_PUBKEY_SIZE 65
#define SECP256K1_SIGNATURE_SIZE 65
#define SECP256K1_MESSAGE_SIZE 32
#define RECID_INDEX 64
#define SHA256_SIZE 32
#define RIPEMD160_SIZE 20
#define SCHNORR_SIGNATURE_SIZE (32 + 64)
#define SCHNORR_PUBKEY_SIZE 32

enum AuthErrorCodeType {
    ERROR_NOT_IMPLEMENTED = 100,
    ERROR_MISMATCHED,
    ERROR_INVALID_ARG,
    ERROR_WRONG_STATE,
    // spawn
    ERROR_SPAWN_INVALID_LENGTH,
    ERROR_SPAWN_SIGN_TOO_LONG,
    ERROR_SPAWN_INVALID_ALGORITHM_ID,
    ERROR_SPAWN_INVALID_SIG,
    ERROR_SPAWN_INVALID_MSG,
    ERROR_SPAWN_INVALID_PUBKEY,
    // schnorr
    ERROR_SCHNORR,
};

typedef int (*validate_signature_t)(void *prefilled_data, const uint8_t *sig,
                                    size_t sig_len, const uint8_t *msg,
                                    size_t msg_len, uint8_t *output,
                                    size_t *output_len);

typedef int (*convert_msg_t)(const uint8_t *msg, size_t msg_len,
                             uint8_t *new_msg, size_t new_msg_len);

int md_string(const mbedtls_md_info_t *md_info, const uint8_t *buf, size_t n,
              unsigned char *output) {
    int err = 0;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    CHECK2(md_info != NULL, MBEDTLS_ERR_MD_BAD_INPUT_DATA);
    err = mbedtls_md_setup(&ctx, md_info, 0);
    CHECK(err);
    err = mbedtls_md_starts(&ctx);
    CHECK(err);
    err = mbedtls_md_update(&ctx, (const unsigned char *)buf, n);
    CHECK(err);
    err = mbedtls_md_finish(&ctx, output);
    CHECK(err);
    err = 0;
exit:
    mbedtls_md_free(&ctx);
    return err;
}

static int _recover_secp256k1_pubkey(const uint8_t *sig, size_t sig_len,
                                     const uint8_t *msg, size_t msg_len,
                                     uint8_t *out_pubkey,
                                     size_t *out_pubkey_size, bool compressed) {
    int ret = 0;

    if (sig_len != SECP256K1_SIGNATURE_SIZE) {
        return ERROR_INVALID_ARG;
    }
    if (msg_len != SECP256K1_MESSAGE_SIZE) {
        return ERROR_INVALID_ARG;
    }

    /* Load signature */
    secp256k1_context context;
    uint8_t secp_data[CKB_SECP256K1_DATA_SIZE];
    ret = ckb_secp256k1_custom_verify_only_initialize(&context, secp_data);
    if (ret != 0) {
        return ret;
    }

    secp256k1_ecdsa_recoverable_signature signature;
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            &context, &signature, sig, sig[RECID_INDEX]) == 0) {
        return ERROR_WRONG_STATE;
    }

    /* Recover pubkey */
    secp256k1_pubkey pubkey;
    if (secp256k1_ecdsa_recover(&context, &pubkey, &signature, msg) != 1) {
        return ERROR_WRONG_STATE;
    }

    unsigned int flag = SECP256K1_EC_COMPRESSED;
    if (compressed) {
        *out_pubkey_size = SECP256K1_PUBKEY_SIZE;
        flag = SECP256K1_EC_COMPRESSED;
    } else {
        *out_pubkey_size = UNCOMPRESSED_SECP256K1_PUBKEY_SIZE;
        flag = SECP256K1_EC_UNCOMPRESSED;
    }
    if (secp256k1_ec_pubkey_serialize(&context, out_pubkey, out_pubkey_size,
                                      &pubkey, flag) != 1) {
        return ERROR_WRONG_STATE;
    }
    return ret;
}

static int _recover_secp256k1_pubkey_btc(const uint8_t *sig, size_t sig_len,
                                         const uint8_t *msg, size_t msg_len,
                                         uint8_t *out_pubkey,
                                         size_t *out_pubkey_size,
                                         bool compressed) {
    (void)compressed;
    int ret = 0;

    if (sig_len != SECP256K1_SIGNATURE_SIZE) {
        return ERROR_INVALID_ARG;
    }
    if (msg_len != SECP256K1_MESSAGE_SIZE) {
        return ERROR_INVALID_ARG;
    }

    // change 1
    int recid = (sig[0] - 27) & 3;
    bool comp = ((sig[0] - 27) & 4) != 0;

    /* Load signature */
    secp256k1_context context;
    uint8_t secp_data[CKB_SECP256K1_DATA_SIZE];
    ret = ckb_secp256k1_custom_verify_only_initialize(&context, secp_data);
    if (ret != 0) {
        return ret;
    }

    secp256k1_ecdsa_recoverable_signature signature;
    // change 2,3
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            &context, &signature, sig + 1, recid) == 0) {
        return ERROR_WRONG_STATE;
    }

    /* Recover pubkey */
    secp256k1_pubkey pubkey;
    if (secp256k1_ecdsa_recover(&context, &pubkey, &signature, msg) != 1) {
        return ERROR_WRONG_STATE;
    }

    unsigned int flag = SECP256K1_EC_COMPRESSED;
    if (comp) {
        *out_pubkey_size = SECP256K1_PUBKEY_SIZE;
        flag = SECP256K1_EC_COMPRESSED;
    } else {
        *out_pubkey_size = UNCOMPRESSED_SECP256K1_PUBKEY_SIZE;
        flag = SECP256K1_EC_UNCOMPRESSED;
    }
    // change 4
    if (secp256k1_ec_pubkey_serialize(&context, out_pubkey, out_pubkey_size,
                                      &pubkey, flag) != 1) {
        return ERROR_WRONG_STATE;
    }
    return ret;
}

int validate_signature_ckb(void *prefilled_data, const uint8_t *sig,
                           size_t sig_len, const uint8_t *msg, size_t msg_len,
                           uint8_t *output, size_t *output_len) {
    int ret = 0;
    if (*output_len < BLAKE160_SIZE) {
        return ERROR_INVALID_ARG;
    }
    uint8_t out_pubkey[SECP256K1_PUBKEY_SIZE];
    size_t out_pubkey_size = SECP256K1_PUBKEY_SIZE;
    ret = _recover_secp256k1_pubkey(sig, sig_len, msg, msg_len, out_pubkey,
                                    &out_pubkey_size, true);
    if (ret != 0) return ret;

    blake2b_state ctx;
    blake2b_init(&ctx, BLAKE2B_BLOCK_SIZE);
    blake2b_update(&ctx, out_pubkey, out_pubkey_size);
    blake2b_final(&ctx, out_pubkey, BLAKE2B_BLOCK_SIZE);

    memcpy(output, out_pubkey, BLAKE160_SIZE);
    *output_len = BLAKE160_SIZE;

    return ret;
}

int validate_signature_eth(void *prefilled_data, const uint8_t *sig,
                           size_t sig_len, const uint8_t *msg, size_t msg_len,
                           uint8_t *output, size_t *output_len) {
    int ret = 0;
    if (*output_len < BLAKE160_SIZE) {
        return SECP256K1_PUBKEY_SIZE;
    }
    uint8_t out_pubkey[UNCOMPRESSED_SECP256K1_PUBKEY_SIZE];
    size_t out_pubkey_size = UNCOMPRESSED_SECP256K1_PUBKEY_SIZE;
    ret = _recover_secp256k1_pubkey(sig, sig_len, msg, msg_len, out_pubkey,
                                    &out_pubkey_size, false);
    if (ret != 0) return ret;

    // here are the 2 differences than validate_signature_secp256k1
    SHA3_CTX sha3_ctx;
    keccak_init(&sha3_ctx);
    keccak_update(&sha3_ctx, &out_pubkey[1], out_pubkey_size - 1);
    keccak_final(&sha3_ctx, out_pubkey);

    memcpy(output, &out_pubkey[12], BLAKE160_SIZE);
    *output_len = BLAKE160_SIZE;

    return ret;
}

int validate_signature_btc(void *prefilled_data, const uint8_t *sig,
                           size_t sig_len, const uint8_t *msg, size_t msg_len,
                           uint8_t *output, size_t *output_len) {
    int err = 0;
    if (*output_len < BLAKE160_SIZE) {
        return SECP256K1_PUBKEY_SIZE;
    }
    uint8_t out_pubkey[UNCOMPRESSED_SECP256K1_PUBKEY_SIZE];
    size_t out_pubkey_size = UNCOMPRESSED_SECP256K1_PUBKEY_SIZE;
    err = _recover_secp256k1_pubkey_btc(sig, sig_len, msg, msg_len, out_pubkey,
                                        &out_pubkey_size, false);
    CHECK(err);

    const mbedtls_md_info_t *md_info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    unsigned char temp[SHA256_SIZE];
    err = md_string(md_info, out_pubkey, out_pubkey_size, temp);
    CHECK(err);

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_RIPEMD160);
    err = md_string(md_info, temp, SHA256_SIZE, temp);
    CHECK(err);

    memcpy(output, temp, BLAKE160_SIZE);
    *output_len = BLAKE160_SIZE;

exit:
    return err;
}

int validate_signature_schnorr(void *prefilled_data, const uint8_t *sig,
                               size_t sig_len, const uint8_t *msg,
                               size_t msg_len, uint8_t *output,
                               size_t *output_len) {
    int err = 0;
    int success = 0;

    if (*output_len < BLAKE160_SIZE) {
        return SECP256K1_PUBKEY_SIZE;
    }
    if (sig_len != SCHNORR_SIGNATURE_SIZE || msg_len != 32) {
        return ERROR_INVALID_ARG;
    }
    secp256k1_context ctx;
    uint8_t secp_data[CKB_SECP256K1_DATA_SIZE];
    err = ckb_secp256k1_custom_verify_only_initialize(&ctx, secp_data);
    if (err != 0) return err;

    secp256k1_xonly_pubkey pk;
    success = secp256k1_xonly_pubkey_parse(&ctx, &pk, sig);
    if (!success) return ERROR_SCHNORR;
    success =
        secp256k1_schnorrsig_verify(&ctx, sig + SCHNORR_PUBKEY_SIZE, msg, &pk);
    if (!success) return ERROR_SCHNORR;

    uint8_t temp[BLAKE2B_BLOCK_SIZE] = {0};
    blake2b_state blake2b_ctx;
    blake2b_init(&blake2b_ctx, BLAKE2B_BLOCK_SIZE);
    blake2b_update(&blake2b_ctx, sig, SCHNORR_PUBKEY_SIZE);
    blake2b_final(&blake2b_ctx, temp, BLAKE2B_BLOCK_SIZE);

    memcpy(output, temp, BLAKE160_SIZE);
    *output_len = BLAKE160_SIZE;

    return 0;
}

int validate_signature_cardano(void *prefilled_data, const uint8_t *sig,
                               size_t sig_len, const uint8_t *msg,
                               size_t msg_len, uint8_t *output,
                               size_t *output_len) {
    int err = 0;

    if (*output_len < BLAKE160_SIZE) {
        return SECP256K1_PUBKEY_SIZE;
    }

    CardanoSignatureData cardano_data;
    CHECK2(get_cardano_data(sig, sig_len, &cardano_data) == CardanoSuccess,
           ERROR_INVALID_ARG);

    CHECK2(memcmp(msg, cardano_data.ckb_sign_msg, msg_len) == 0,
           ERROR_INVALID_ARG);

    int suc = ed25519_verify(cardano_data.signature, cardano_data.sign_message,
                             CARDANO_LOCK_SIGNATURE_MESSAGE_SIZE,
                             cardano_data.public_key);
    CHECK2(suc == 1, ERROR_WRONG_STATE);

    blake2b_state ctx;
    uint8_t pubkey_hash[BLAKE2B_BLOCK_SIZE] = {0};
    blake2b_init(&ctx, BLAKE2B_BLOCK_SIZE);
    blake2b_update(&ctx, cardano_data.public_key,
                   sizeof(cardano_data.public_key));
    blake2b_final(&ctx, pubkey_hash, sizeof(pubkey_hash));

    memcpy(output, pubkey_hash, BLAKE160_SIZE);
    *output_len = BLAKE160_SIZE;
exit:
    return err;
}

int convert_copy(const uint8_t *msg, size_t msg_len, uint8_t *new_msg,
                 size_t new_msg_len) {
    if (msg_len != new_msg_len || msg_len != BLAKE2B_BLOCK_SIZE)
        return ERROR_INVALID_ARG;
    memcpy(new_msg, msg, msg_len);
    return 0;
}

int convert_eth_message(const uint8_t *msg, size_t msg_len, uint8_t *new_msg,
                        size_t new_msg_len) {
    if (msg_len != new_msg_len || msg_len != BLAKE2B_BLOCK_SIZE)
        return ERROR_INVALID_ARG;

    SHA3_CTX sha3_ctx;
    keccak_init(&sha3_ctx);
    /* personal hash, ethereum prefix  \u0019Ethereum Signed Message:\n32  */
    unsigned char eth_prefix[28];
    eth_prefix[0] = 0x19;
    memcpy(eth_prefix + 1, "Ethereum Signed Message:\n32", 27);

    keccak_update(&sha3_ctx, eth_prefix, 28);
    keccak_update(&sha3_ctx, (unsigned char *)msg, 32);
    keccak_final(&sha3_ctx, new_msg);
    return 0;
}

int convert_tron_message(const uint8_t *msg, size_t msg_len, uint8_t *new_msg,
                         size_t new_msg_len) {
    if (msg_len != new_msg_len || msg_len != BLAKE2B_BLOCK_SIZE)
        return ERROR_INVALID_ARG;

    SHA3_CTX sha3_ctx;
    keccak_init(&sha3_ctx);
    /* ASCII code for tron prefix \x19TRON Signed Message:\n32, refer
     * https://github.com/tronprotocol/tips/issues/104 */
    unsigned char tron_prefix[24];
    tron_prefix[0] = 0x19;
    memcpy(tron_prefix + 1, "TRON Signed Message:\n32", 23);

    keccak_update(&sha3_ctx, tron_prefix, 24);
    keccak_update(&sha3_ctx, (unsigned char *)msg, 32);
    keccak_final(&sha3_ctx, new_msg);
    return 0;
}

static void bin_to_hex(const uint8_t *source, uint8_t *dest, size_t len) {
    const static uint8_t HEX_TABLE[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    for (int i = 0; i < len; i++) {
        dest[i * 2] = HEX_TABLE[source[i] >> 4];
        dest[i * 2 + 1] = HEX_TABLE[source[i] & 0x0F];
    }
}

static void split_hex_hash(const uint8_t *source, unsigned char *dest) {
    int i;
    char hex_chars[] = "0123456789abcdef";

    for (i = 0; i < BLAKE2B_BLOCK_SIZE; i++) {
        if (i > 0 && i % 6 == 0) {
            *(dest++) = ' ';
        }
        *(dest++) = hex_chars[source[i] / 16];
        *(dest++) = hex_chars[source[i] % 16];
    }
}

int convert_eos_message(const uint8_t *msg, size_t msg_len, uint8_t *new_msg,
                        size_t new_msg_len) {
    int err = 0;
    if (msg_len != new_msg_len || msg_len != BLAKE2B_BLOCK_SIZE)
        return ERROR_INVALID_ARG;
    int split_message_len = BLAKE2B_BLOCK_SIZE * 2 + 5;
    unsigned char splited_message[split_message_len];
    /* split message to words length <= 12 */
    split_hex_hash(msg, splited_message);

    const mbedtls_md_info_t *md_info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    err = md_string(md_info, msg, msg_len, new_msg);
    if (err != 0) return err;
    return 0;
}

#define MESSAGE_HEX_LEN 64
int convert_btc_message_variant(const uint8_t *msg, size_t msg_len,
                                uint8_t *new_msg, size_t new_msg_len,
                                const char *magic, const uint8_t magic_len) {
    int err = 0;
    if (msg_len != new_msg_len || msg_len != SHA256_SIZE)
        return ERROR_INVALID_ARG;

    uint8_t temp[MESSAGE_HEX_LEN];
    bin_to_hex(msg, temp, 32);

    // len of magic + magic string + len of message, size is 26 Byte
    uint8_t new_magic[magic_len + 2];
    new_magic[0] = magic_len;  // MESSAGE_MAGIC length
    memcpy(&new_magic[1], magic, magic_len);
    new_magic[magic_len + 1] = MESSAGE_HEX_LEN;  // message length

    const mbedtls_md_info_t *md_info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    /* Calculate signature message */
    uint8_t temp2[magic_len + 2 + MESSAGE_HEX_LEN];
    uint32_t temp2_size = magic_len + 2 + MESSAGE_HEX_LEN;
    memcpy(temp2, new_magic, magic_len + 2);
    memcpy(temp2 + magic_len + 2, temp, MESSAGE_HEX_LEN);
    err = md_string(md_info, temp2, temp2_size, new_msg);
    if (err != 0) return err;
    err = md_string(md_info, new_msg, SHA256_SIZE, new_msg);
    if (err != 0) return err;
    return 0;
}

const char BTC_MESSAGE_MAGIC[25] = "Bitcoin Signed Message:\n";
const int8_t BTC_MAGIC_LEN = 24;

int convert_btc_message(const uint8_t *msg, size_t msg_len, uint8_t *new_msg,
                        size_t new_msg_len) {
    return convert_btc_message_variant(msg, msg_len, new_msg, new_msg_len,
                                       BTC_MESSAGE_MAGIC, BTC_MAGIC_LEN);
}

const char DOGE_MESSAGE_MAGIC[26] = "Dogecoin Signed Message:\n";
const int8_t DOGE_MAGIC_LEN = 25;

int convert_doge_message(const uint8_t *msg, size_t msg_len, uint8_t *new_msg,
                         size_t new_msg_len) {
    return convert_btc_message_variant(msg, msg_len, new_msg, new_msg_len,
                                       DOGE_MESSAGE_MAGIC, DOGE_MAGIC_LEN);
}

const char LITE_MESSAGE_MAGIC[26] = "Litecoin Signed Message:\n";
const int8_t LITE_MAGIC_LEN = 25;

int convert_litecoin_message(const uint8_t *msg, size_t msg_len,
                             uint8_t *new_msg, size_t new_msg_len) {
    return convert_btc_message_variant(msg, msg_len, new_msg, new_msg_len,
                                       LITE_MESSAGE_MAGIC, LITE_MAGIC_LEN);
}

bool is_lock_script_hash_present(uint8_t *lock_script_hash) {
    int err = 0;
    size_t i = 0;
    while (true) {
        uint8_t buff[BLAKE2B_BLOCK_SIZE];
        uint64_t len = BLAKE2B_BLOCK_SIZE;
        err = ckb_checked_load_cell_by_field(buff, &len, 0, i, CKB_SOURCE_INPUT,
                                             CKB_CELL_FIELD_LOCK_HASH);
        if (err == CKB_INDEX_OUT_OF_BOUND) {
            break;
        }
        if (err != 0) {
            break;
        }

        if (memcmp(lock_script_hash, buff, BLAKE160_SIZE) == 0) {
            return true;
        }
        i += 1;
    }
    return false;
}

static int verify(uint8_t *pubkey_hash, const uint8_t *sig, uint32_t sig_len,
                  const uint8_t *msg, uint32_t msg_len,
                  validate_signature_t func, convert_msg_t convert) {
    int err = 0;
    uint8_t new_msg[BLAKE2B_BLOCK_SIZE];

    // for md_string
    unsigned char alloc_buff[1024];
    mbedtls_memory_buffer_alloc_init(alloc_buff, sizeof(alloc_buff));

    err = convert(msg, msg_len, new_msg, sizeof(new_msg));
    CHECK(err);

    uint8_t output_pubkey_hash[BLAKE160_SIZE];
    size_t output_len = BLAKE160_SIZE;
    err = func(NULL, sig, sig_len, new_msg, sizeof(new_msg), output_pubkey_hash,
               &output_len);
    CHECK(err);

    int same = memcmp(pubkey_hash, output_pubkey_hash, BLAKE160_SIZE);
    CHECK2(same == 0, ERROR_MISMATCHED);

exit:
    return err;
}

// origin:
// https://github.com/nervosnetwork/ckb-system-scripts/blob/master/c/secp256k1_blake160_multisig_all.c
// Script args validation errors
#define ERROR_INVALID_RESERVE_FIELD -41
#define ERROR_INVALID_PUBKEYS_CNT -42
#define ERROR_INVALID_THRESHOLD -43
#define ERROR_INVALID_REQUIRE_FIRST_N -44
// Multi-sigining validation errors
#define ERROR_MULTSIG_SCRIPT_HASH -51
#define ERROR_VERIFICATION -52
#define ERROR_WITNESS_SIZE -22
#define ERROR_SECP_PARSE_SIGNATURE -14
#define ERROR_SECP_RECOVER_PUBKEY -11
#define ERROR_SECP_SERIALIZE_PUBKEY -15

#define FLAGS_SIZE 4
#define SIGNATURE_SIZE 65
#define PUBKEY_SIZE 33

int verify_multisig(const uint8_t *lock_bytes, size_t lock_bytes_len,
                    const uint8_t *message, const uint8_t *hash) {
    int ret;
    uint8_t temp[PUBKEY_SIZE];

    // Extract multisig script flags.
    uint8_t pubkeys_cnt = lock_bytes[3];
    uint8_t threshold = lock_bytes[2];
    uint8_t require_first_n = lock_bytes[1];
    uint8_t reserved_field = lock_bytes[0];
    if (reserved_field != 0) {
        return ERROR_INVALID_RESERVE_FIELD;
    }
    if (pubkeys_cnt == 0) {
        return ERROR_INVALID_PUBKEYS_CNT;
    }
    if (threshold > pubkeys_cnt) {
        return ERROR_INVALID_THRESHOLD;
    }
    if (threshold == 0) {
        return ERROR_INVALID_THRESHOLD;
    }
    if (require_first_n > threshold) {
        return ERROR_INVALID_REQUIRE_FIRST_N;
    }
    // Based on the number of public keys and thresholds, we can calculate
    // the required length of the lock field.
    size_t multisig_script_len = FLAGS_SIZE + BLAKE160_SIZE * pubkeys_cnt;
    size_t signatures_len = SIGNATURE_SIZE * threshold;
    size_t required_lock_len = multisig_script_len + signatures_len;
    if (lock_bytes_len != required_lock_len) {
        return ERROR_WITNESS_SIZE;
    }

    // Perform hash check of the `multisig_script` part, notice the signature
    // part is not included here.
    blake2b_state blake2b_ctx;
    blake2b_init(&blake2b_ctx, BLAKE2B_BLOCK_SIZE);
    blake2b_update(&blake2b_ctx, lock_bytes, multisig_script_len);
    blake2b_final(&blake2b_ctx, temp, BLAKE2B_BLOCK_SIZE);

    if (memcmp(hash, temp, BLAKE160_SIZE) != 0) {
        return ERROR_MULTSIG_SCRIPT_HASH;
    }

    // Verify threshold signatures, threshold is a uint8_t, at most it is
    // 255, meaning this array will definitely have a reasonable upper bound.
    // Also this code uses C99's new feature to allocate a variable length
    // array.
    uint8_t used_signatures[pubkeys_cnt];
    memset(used_signatures, 0, pubkeys_cnt);

    // We are using bitcoin's [secp256k1
    // library](https://github.com/bitcoin-core/secp256k1) for signature
    // verification here. To the best of our knowledge, this is an unmatched
    // advantage of CKB: you can ship cryptographic algorithm within your smart
    // contract, you don't have to wait for the foundation to ship a new
    // cryptographic algorithm. You can just build and ship your own.
    secp256k1_context context;
    uint8_t secp_data[CKB_SECP256K1_DATA_SIZE];
    ret = ckb_secp256k1_custom_verify_only_initialize(&context, secp_data);
    if (ret != 0) return ret;

    // We will perform *threshold* number of signature verifications here.
    for (size_t i = 0; i < threshold; i++) {
        // Load signature
        secp256k1_ecdsa_recoverable_signature signature;
        size_t signature_offset = multisig_script_len + i * SIGNATURE_SIZE;
        if (secp256k1_ecdsa_recoverable_signature_parse_compact(
                &context, &signature, &lock_bytes[signature_offset],
                lock_bytes[signature_offset + RECID_INDEX]) == 0) {
            return ERROR_SECP_PARSE_SIGNATURE;
        }

        // verify signature and Recover pubkey
        secp256k1_pubkey pubkey;
        if (secp256k1_ecdsa_recover(&context, &pubkey, &signature, message) !=
            1) {
            return ERROR_SECP_RECOVER_PUBKEY;
        }

        // Calculate the blake160 hash of the derived public key
        size_t pubkey_size = PUBKEY_SIZE;
        if (secp256k1_ec_pubkey_serialize(&context, temp, &pubkey_size, &pubkey,
                                          SECP256K1_EC_COMPRESSED) != 1) {
            return ERROR_SECP_SERIALIZE_PUBKEY;
        }

        unsigned char calculated_pubkey_hash[BLAKE2B_BLOCK_SIZE];
        blake2b_state blake2b_ctx;
        blake2b_init(&blake2b_ctx, BLAKE2B_BLOCK_SIZE);
        blake2b_update(&blake2b_ctx, temp, PUBKEY_SIZE);
        blake2b_final(&blake2b_ctx, calculated_pubkey_hash, BLAKE2B_BLOCK_SIZE);

        // Check if this signature is signed with one of the provided public
        // key.
        uint8_t matched = 0;
        for (size_t i = 0; i < pubkeys_cnt; i++) {
            if (used_signatures[i] == 1) {
                continue;
            }
            if (memcmp(&lock_bytes[FLAGS_SIZE + i * BLAKE160_SIZE],
                       calculated_pubkey_hash, BLAKE160_SIZE) != 0) {
                continue;
            }
            matched = 1;
            used_signatures[i] = 1;
            break;
        }

        // If the signature doesn't match any of the provided public key, the
        // script will exit with an error.
        if (matched != 1) {
            return ERROR_VERIFICATION;
        }
    }

    // The above scheme just ensures that a *threshold* number of signatures
    // have successfully been verified, and they all come from the provided
    // public keys. However, the multisig script might also require some numbers
    // of public keys to always be signed for the script to pass verification.
    // This is indicated via the *required_first_n* flag. Here we also checks to
    // see that this rule is also satisfied.
    for (size_t i = 0; i < require_first_n; i++) {
        if (used_signatures[i] != 1) {
            return ERROR_VERIFICATION;
        }
    }

    return 0;
}

// dynamic linking entry
__attribute__((visibility("default"))) int ckb_auth_validate(
    uint8_t auth_algorithm_id, const uint8_t *signature,
    uint32_t signature_size, const uint8_t *message, uint32_t message_size,
    uint8_t *pubkey_hash, uint32_t pubkey_hash_size) {
    int err = 0;
    CHECK2(signature != NULL, ERROR_INVALID_ARG);
    CHECK2(message != NULL, ERROR_INVALID_ARG);
    CHECK2(message_size > 0, ERROR_INVALID_ARG);
    CHECK2(pubkey_hash_size == BLAKE160_SIZE, ERROR_INVALID_ARG);

    if (auth_algorithm_id == AuthAlgorithmIdCkb) {
        CHECK2(signature_size == SECP256K1_SIGNATURE_SIZE, ERROR_INVALID_ARG);
        err = verify(pubkey_hash, signature, signature_size, message,
                     message_size, validate_signature_ckb, convert_copy);
        CHECK(err);
    } else if (auth_algorithm_id == AuthAlgorithmIdEthereum) {
        CHECK2(signature_size == SECP256K1_SIGNATURE_SIZE, ERROR_INVALID_ARG);
        err = verify(pubkey_hash, signature, signature_size, message,
                     message_size, validate_signature_eth, convert_eth_message);
        CHECK(err);
    } else if (auth_algorithm_id == AuthAlgorithmIdEos) {
        CHECK2(signature_size == SECP256K1_SIGNATURE_SIZE, ERROR_INVALID_ARG);
        err = verify(pubkey_hash, signature, signature_size, message,
                     message_size, validate_signature_eth, convert_eos_message);
        CHECK(err);
    } else if (auth_algorithm_id == AuthAlgorithmIdTron) {
        CHECK2(signature_size == SECP256K1_SIGNATURE_SIZE, ERROR_INVALID_ARG);
        err =
            verify(pubkey_hash, signature, signature_size, message,
                   message_size, validate_signature_eth, convert_tron_message);
        CHECK(err);
    } else if (auth_algorithm_id == AuthAlgorithmIdBitcoin) {
        err = verify(pubkey_hash, signature, signature_size, message,
                     message_size, validate_signature_btc, convert_btc_message);
        CHECK(err);
    } else if (auth_algorithm_id == AuthAlgorithmIdDogecoin) {
        err =
            verify(pubkey_hash, signature, signature_size, message,
                   message_size, validate_signature_btc, convert_doge_message);
        CHECK(err);
    } else if (auth_algorithm_id == AuthAlgorithmIdLitecoin) {
        err = verify(pubkey_hash, signature, signature_size, message,
                     message_size, validate_signature_btc,
                     convert_litecoin_message);
        CHECK(err);
    } else if (auth_algorithm_id == AuthAlgorithmIdCkbMultisig) {
        err = verify_multisig(signature, signature_size, message, pubkey_hash);
        CHECK(err);
    } else if (auth_algorithm_id == AuthAlgorithmIdSchnorr) {
        err = verify(pubkey_hash, signature, signature_size, message,
                     message_size, validate_signature_schnorr, convert_copy);
        CHECK(err);
    } else if (auth_algorithm_id == AuthAlgorithmIdCardano) {
        err = verify(pubkey_hash, signature, signature_size, message,
                     message_size, validate_signature_cardano, convert_copy);
        CHECK(err);
    } else if (auth_algorithm_id == AuthAlgorithmIdOwnerLock) {
        CHECK2(is_lock_script_hash_present(pubkey_hash), ERROR_MISMATCHED);
        err = 0;
    } else {
        CHECK2(false, ERROR_NOT_IMPLEMENTED);
    }
exit:
    return err;
}

#define OFFSETOF(TYPE, ELEMENT) ((size_t) & (((TYPE *)0)->ELEMENT))
#define PT_DYNAMIC 2

/* See https://docs.oracle.com/cd/E23824_01/html/819-0690/chapter6-42444.html for details */
#define DT_RELA 7
#define DT_RELACOUNT 0x6ffffff9
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define DT_PLTREL 20
#define DT_SYMTAB 6
#define DT_SYMENT 11

typedef struct {
    uint64_t type;
    uint64_t value;
} Elf64_Dynamic;

#ifdef CKB_USE_SIM
int simulator_main(int argc, char *argv[]) {
#else
// spawn entry
int main(int argc, char *argv[]) {
// fix error:
// c/auth.c:810:50: error: array subscript 0 is outside array bounds of
// 'uint64_t[0]' {aka 'long unsigned int[]'} [-Werror=array-bounds]
//   810 |     Elf64_Phdr *program_headers = (Elf64_Phdr *)(*phoff);
//       |                                                 ~^~~~~~~
#if defined(__GNUC__) && (__GNUC__ >= 12)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
    uint64_t *phoff = (uint64_t *)OFFSETOF(Elf64_Ehdr, e_phoff);
    uint16_t *phnum = (uint16_t *)OFFSETOF(Elf64_Ehdr, e_phnum);
    Elf64_Phdr *program_headers = (Elf64_Phdr *)(*phoff);
    for (int i = 0; i < *phnum; i++) {
        Elf64_Phdr *program_header = &program_headers[i];
        if (program_header->p_type == PT_DYNAMIC) {
            Elf64_Dynamic *d = (Elf64_Dynamic *)program_header->p_vaddr;
            uint64_t rela_address = 0;
            uint64_t rela_count = 0;
            uint64_t jmprel_address = 0;
            uint64_t pltrel_size = 0;
            uint64_t pltrel = 0;
            uint64_t symtab_address = 0;
            uint64_t symtab_entry_size = 0;
            while (d->type != 0) {
                switch (d->type) {
                  case DT_RELA:
                    rela_address = d->value;
                    break;
                  case DT_RELACOUNT:
                    rela_count = d->value;
                    break;
                  case DT_JMPREL:
                    jmprel_address = d->value;
                    break;
                  case DT_PLTRELSZ:
                    pltrel_size = d->value;
                    break;
                  case DT_PLTREL:
                    pltrel = d->value;
                    break;
                  case DT_SYMTAB:
                    symtab_address = d->value;
                    break;
                  case DT_SYMENT:
                    symtab_entry_size = d->value;
                    break;
                }
                d++;
            }
            if (rela_address > 0 && rela_count > 0) {
                Elf64_Rela *relocations = (Elf64_Rela *)rela_address;
                for (int j = 0; j < rela_count; j++) {
                    Elf64_Rela *relocation = &relocations[j];
                    if (relocation->r_info != R_RISCV_RELATIVE) {
                        return ERROR_INVALID_ELF;
                    }
                    *((uint64_t *)(relocation->r_offset)) =
                        (uint64_t)(relocation->r_addend);
                }
            }
            if (jmprel_address > 0 && pltrel_size > 0 && pltrel == DT_RELA && symtab_address > 0) {
                if (pltrel_size % sizeof(Elf64_Rela) != 0) {
                    return ERROR_INVALID_ELF;
                }
                if (symtab_entry_size != sizeof(Elf64_Sym)) {
                    return ERROR_INVALID_ELF;
                }
                Elf64_Rela *relocations = (Elf64_Rela *) jmprel_address;
                Elf64_Sym *symbols = (Elf64_Sym *) symtab_address;
                for (int j = 0; j < pltrel_size / sizeof(Elf64_Rela); j++) {
                    Elf64_Rela *relocation = &relocations[j];
                    uint32_t idx = (uint32_t) (relocation->r_info >> 32);
                    uint32_t t = (uint32_t) relocation->r_info;
                    if (t != R_RISCV_JUMP_SLOT) {
                        return ERROR_INVALID_ELF;
                    }
                    Elf64_Sym *sym = &symbols[idx];
                    *((uint64_t *)(relocation->r_offset)) = sym->st_value;
                }
            }
        }
    }

#if defined(__GNUC__) && (__GNUC__ >= 12)
#pragma GCC diagnostic pop
#endif

#endif

    int err = 0;

    if (argc != 4) {
        return -1;
    }

#define ARGV_ALGORITHM_ID argv[0]
#define ARGV_SIGNATURE argv[1]
#define ARGV_MESSAGE argv[2]
#define ARGV_PUBKEY_HASH argv[3]

    uint32_t algorithm_id_len = strlen(ARGV_ALGORITHM_ID);
    uint32_t signature_len = strlen(ARGV_SIGNATURE);
    uint32_t message_len = strlen(ARGV_MESSAGE);
    uint32_t pubkey_hash_len = strlen(ARGV_PUBKEY_HASH);

    if (algorithm_id_len != 2 || signature_len % 2 != 0 ||
        message_len != BLAKE2B_BLOCK_SIZE * 2 ||
        pubkey_hash_len != BLAKE160_SIZE * 2) {
        return ERROR_SPAWN_INVALID_LENGTH;
    }

    // Limit the maximum size of signature
    if (signature_len > 1024 * 64 * 2) {
        return ERROR_SPAWN_SIGN_TOO_LONG;
    }

    uint8_t algorithm_id = 0;
    uint8_t signature[signature_len / 2];
    uint8_t message[BLAKE2B_BLOCK_SIZE];
    uint8_t pubkey_hash[BLAKE160_SIZE];

    // auth algorithm id
    CHECK2(
        !ckb_hex2bin(ARGV_ALGORITHM_ID, &algorithm_id, 1, &algorithm_id_len) &&
            algorithm_id_len == 1,
        ERROR_SPAWN_INVALID_ALGORITHM_ID);

    // signature
    CHECK2(
        !ckb_hex2bin(ARGV_SIGNATURE, signature, signature_len, &signature_len),
        ERROR_SPAWN_INVALID_SIG);

    // message
    CHECK2(!ckb_hex2bin(ARGV_MESSAGE, message, message_len, &message_len) &&
               message_len == BLAKE2B_BLOCK_SIZE,
           ERROR_SPAWN_INVALID_MSG);

    // public key hash
    CHECK2(!ckb_hex2bin(ARGV_PUBKEY_HASH, pubkey_hash, pubkey_hash_len,
                        &pubkey_hash_len) &&
               pubkey_hash_len == BLAKE160_SIZE,
           ERROR_SPAWN_INVALID_PUBKEY);

    err = ckb_auth_validate(algorithm_id, signature, signature_len, message,
                            message_len, pubkey_hash, pubkey_hash_len);
    CHECK(err);

exit:
    return err;

#undef ARGV_ALGORITHM_ID
#undef ARGV_SIGNATURE
#undef ARGV_MESSAGE
#undef ARGV_PUBKEY_HASH
}
