// SPDX-License-Identifier: Apache-2.0

/*
 * Frozen old-consumer declarations. This translation unit deliberately does
 * not include current Kungfu headers: it proves that the v1/v2/v3 table
 * prefixes and bootstrap negotiation still work for independently compiled C.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define KF_OLD_CALL __cdecl
#else
#define KF_OLD_CALL
#endif

enum {
  KF_OLD_OK = 0,
  KF_OLD_INVALID_ARGUMENT = 1,
  KF_OLD_UNSUPPORTED_VERSION = 2,
};

typedef void(KF_OLD_CALL *kf_old_slot)(void);

typedef struct kf_embedding_api_v1_old {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t capabilities;
  kf_old_slot context_open;
  kf_old_slot context_capabilities;
  kf_old_slot context_close;
  kf_old_slot reader_open;
  kf_old_slot reader_read_batch;
  kf_old_slot reader_release_batch;
  kf_old_slot reader_close;
} kf_embedding_api_v1_old;

typedef struct kf_embedding_api_v2_old {
  kf_embedding_api_v1_old v1;
  kf_old_slot storage_fsck;
  kf_old_slot report_release;
} kf_embedding_api_v2_old;

typedef struct kf_embedding_api_v3_old {
  kf_embedding_api_v2_old v2;
  kf_old_slot decode_frame_json;
  kf_old_slot frame_checksum;
} kf_embedding_api_v3_old;

typedef struct kf_native_storage_api_v1_old {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t capabilities;
  kf_old_slot context_open;
  kf_old_slot context_capabilities;
  kf_old_slot context_last_error;
  kf_old_slot context_close;
  kf_old_slot execute;
  kf_old_slot release_result;
} kf_native_storage_api_v1_old;

extern int32_t KF_OLD_CALL kungfu_embedding_get_api(uint32_t requested_version, uint32_t caller_struct_size,
                                                    void *out_api);
extern int32_t KF_OLD_CALL kungfu_native_storage_get_api(uint32_t requested_version, uint32_t caller_struct_size,
                                                         kf_native_storage_api_v1_old *out_api);

static int require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    return 0;
  }
  return 1;
}

int main(void) {
  kf_embedding_api_v1_old embedding_v1;
  kf_embedding_api_v2_old embedding_v2;
  kf_embedding_api_v3_old embedding_v3;
  kf_native_storage_api_v1_old storage_v1;
  memset(&embedding_v1, 0, sizeof(embedding_v1));
  memset(&embedding_v2, 0, sizeof(embedding_v2));
  memset(&embedding_v3, 0, sizeof(embedding_v3));
  memset(&storage_v1, 0, sizeof(storage_v1));

  if (!require(kungfu_embedding_get_api(1, sizeof(embedding_v1), &embedding_v1) == KF_OLD_OK,
               "old embedding v1 consumer rejected") ||
      !require(embedding_v1.abi_version == 1 && embedding_v1.struct_size == sizeof(embedding_v1),
               "embedding v1 prefix changed") ||
      !require((embedding_v1.capabilities & UINT64_C(3)) == UINT64_C(3), "embedding v1 capabilities regressed") ||
      !require(kungfu_embedding_get_api(2, sizeof(embedding_v2), &embedding_v2) == KF_OLD_OK,
               "old embedding v2 consumer rejected") ||
      !require(embedding_v2.v1.abi_version == 2 && embedding_v2.v1.struct_size == sizeof(embedding_v2),
               "embedding v2 prefix changed") ||
      !require((embedding_v2.v1.capabilities & UINT64_C(7)) == UINT64_C(7),
               "embedding v2 capability increment regressed") ||
      !require(kungfu_embedding_get_api(3, sizeof(embedding_v3), &embedding_v3) == KF_OLD_OK,
               "old embedding v3 consumer rejected") ||
      !require(embedding_v3.v2.v1.abi_version == 3 && embedding_v3.v2.v1.struct_size == sizeof(embedding_v3),
               "embedding v3 prefix changed") ||
      !require((embedding_v3.v2.v1.capabilities & UINT64_C(15)) == UINT64_C(15),
               "embedding v3 capability increment regressed") ||
      !require(kungfu_embedding_get_api(1, sizeof(embedding_v1) - 1, &embedding_v1) == KF_OLD_INVALID_ARGUMENT,
               "embedding undersized caller accepted") ||
      !require(kungfu_embedding_get_api(99, sizeof(embedding_v3), &embedding_v3) == KF_OLD_UNSUPPORTED_VERSION,
               "embedding unknown version accepted") ||
      !require(kungfu_native_storage_get_api(1, sizeof(storage_v1), &storage_v1) == KF_OLD_OK,
               "old native-storage v1 consumer rejected") ||
      !require(storage_v1.abi_version == 1 && storage_v1.struct_size == sizeof(storage_v1),
               "native-storage v1 prefix changed") ||
      !require((storage_v1.capabilities & UINT64_C(63)) == UINT64_C(63), "native-storage v1 capabilities regressed") ||
      !require(kungfu_native_storage_get_api(1, sizeof(storage_v1) - 1, &storage_v1) == KF_OLD_INVALID_ARGUMENT,
               "native-storage undersized caller accepted") ||
      !require(kungfu_native_storage_get_api(99, sizeof(storage_v1), &storage_v1) == KF_OLD_UNSUPPORTED_VERSION,
               "native-storage unknown version accepted")) {
    return 1;
  }
  return 0;
}
