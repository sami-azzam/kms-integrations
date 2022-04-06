// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kmsp11/operation/kms_digesting_signer.h"

#include "kmsp11/operation/kms_prehashed_signer.h"
#include "kmsp11/operation/preconditions.h"
#include "kmsp11/operation/rsassa_pkcs1.h"
#include "kmsp11/operation/rsassa_pss.h"
#include "kmsp11/operation/rsassa_raw_pkcs1.h"
#include "kmsp11/util/crypto_utils.h"
#include "kmsp11/util/errors.h"
#include "kmsp11/util/status_macros.h"

namespace kmsp11 {

absl::StatusOr<std::unique_ptr<SignerInterface>> KmsDigestingSigner::New(
    std::shared_ptr<Object> key, std::unique_ptr<SignerInterface> inner_signer,
    const CK_MECHANISM* mechanism) {
  ASSIGN_OR_RETURN(const EVP_MD* md, DigestForMechanism(mechanism->mechanism));
  return std::unique_ptr<SignerInterface>(
      new KmsDigestingSigner(std::move(inner_signer), md));
}

absl::StatusOr<std::unique_ptr<SignerInterface>> KmsDigestingSigner::New(
    std::shared_ptr<Object> key, const CK_MECHANISM* mechanism) {
  CK_MECHANISM inner_mechanism;
  std::unique_ptr<SignerInterface> inner_signer;
  switch (mechanism->mechanism) {
    case CKM_SHA256_RSA_PKCS:
    case CKM_SHA512_RSA_PKCS: {
      inner_mechanism = {CKM_RSA_PKCS, nullptr, 0};
      RETURN_IF_ERROR(EnsureNoParameters(mechanism));
      RETURN_IF_ERROR(CheckKeyPreconditions(CKK_RSA, CKO_PRIVATE_KEY,
                                            mechanism->mechanism, key.get()));
      if (IsRawRsaAlgorithm(key->algorithm().algorithm)) {
        ASSIGN_OR_RETURN(inner_signer,
                         RsaRawPkcs1Signer::New(key, &inner_mechanism));
        break;
      }
      ASSIGN_OR_RETURN(
          inner_signer,
          RsaPkcs1Signer::New(key, &inner_mechanism, ExpectedInput::kDigest));
      break;
    }
    case CKM_SHA256_RSA_PKCS_PSS:
    case CKM_SHA512_RSA_PKCS_PSS: {
      inner_mechanism = {CKM_RSA_PKCS_PSS, mechanism->pParameter,
                         mechanism->ulParameterLen};
      ASSIGN_OR_RETURN(inner_signer, RsaPssSigner::New(key, &inner_mechanism));
      break;
    }
    default:
      return NewInternalError(
          absl::StrFormat("KmsDigestingSigner does not support mechanism %#x",
                          mechanism->mechanism),
          SOURCE_LOCATION);
  }

  ASSIGN_OR_RETURN(const EVP_MD* md, DigestForMechanism(mechanism->mechanism));

  return std::unique_ptr<SignerInterface>(
      new KmsDigestingSigner(std::move(inner_signer), md));
}

absl::Status KmsDigestingSigner::Sign(KmsClient* client,
                                      absl::Span<const uint8_t> data,
                                      absl::Span<uint8_t> signature) {
  if (md_ctx_) {
    return FailedPreconditionError(
        "Sign cannot be used to terminate a multi-part signing operation",
        CKR_FUNCTION_FAILED, SOURCE_LOCATION);
  }

  const size_t md_size = EVP_MD_size(md_);
  std::vector<uint8_t> evp_digest(md_size);
  unsigned int digest_len;
  bssl::UniquePtr<EVP_MD_CTX> ctx(EVP_MD_CTX_new());
  if (EVP_Digest(data.data(), data.size(), evp_digest.data(), &digest_len, md_,
                 nullptr) != 1) {
    return NewInternalError(
        absl::StrFormat(
            "failed while computing EVP digest with digest size %d: %s",
            md_size, SslErrorToString()),
        SOURCE_LOCATION);
  }

  if (digest_len != md_size) {
    return NewInternalError(
        absl::StrFormat(
            "computed digest has incorrect size (got %d, want %d): %s",
            digest_len, md_size, SslErrorToString()),
        SOURCE_LOCATION);
  }

  if (IsRawRsaAlgorithm(object()->algorithm().algorithm)) {
    ASSIGN_OR_RETURN(std::vector<uint8_t> digest_info,
                     BuildRsaDigestInfo(EVP_MD_type(md_), evp_digest));
    return inner_signer_->Sign(client, digest_info, signature);
  }

  return inner_signer_->Sign(client, evp_digest, signature);
}

absl::Status KmsDigestingSigner::SignUpdate(KmsClient* client,
                                            absl::Span<const uint8_t> data) {
  if (!md_ctx_) {
    md_ctx_ = bssl::UniquePtr<EVP_MD_CTX>(EVP_MD_CTX_new());
    if (EVP_DigestInit(md_ctx_.get(), md_) != 1) {
      return NewInternalError(
          absl::StrFormat(
              "failed while initializing EVP digest with digest size %d: %s",
              EVP_MD_size(md_), SslErrorToString()),
          SOURCE_LOCATION);
    }
  }

  if (EVP_DigestUpdate(md_ctx_.get(), data.data(), data.size()) != 1) {
    return NewInternalError(
        absl::StrCat("failed while updating EVP digest with input data: ",
                     SslErrorToString()),
        SOURCE_LOCATION);
  }

  return absl::OkStatus();
}

absl::Status KmsDigestingSigner::SignFinal(KmsClient* client,
                                           absl::Span<uint8_t> signature) {
  if (!md_ctx_) {
    return FailedPreconditionError(
        "SignUpdate needs to be called prior to terminating a multi-part "
        "signing operation",
        CKR_FUNCTION_FAILED, SOURCE_LOCATION);
  }

  std::vector<uint8_t> evp_digest(EVP_MD_size(md_));
  unsigned int digest_len;
  if (EVP_DigestFinal(md_ctx_.get(), evp_digest.data(), &digest_len) != 1) {
    return NewInternalError(absl::StrCat("failed while finalizing EVP digest: ",
                                         SslErrorToString()),
                            SOURCE_LOCATION);
  }

  if (digest_len != EVP_MD_size(md_)) {
    return NewInternalError(
        absl::StrFormat(
            "computed digest has incorrect size (got %d, want %d): %s",
            digest_len, EVP_MD_size(md_), SslErrorToString()),
        SOURCE_LOCATION);
  }

  if (IsRawRsaAlgorithm(object()->algorithm().algorithm)) {
    ASSIGN_OR_RETURN(std::vector<uint8_t> digest_info,
                     BuildRsaDigestInfo(EVP_MD_type(md_), evp_digest));
    return inner_signer_->Sign(client, digest_info, signature);
  }

  return inner_signer_->Sign(client, evp_digest, signature);
}

size_t KmsDigestingSigner::signature_length() {
  return inner_signer_->signature_length();
}

}  // namespace kmsp11
