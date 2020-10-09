#include "kmsp11/token.h"

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "glog/logging.h"
#include "kmsp11/algorithm_details.h"
#include "kmsp11/cert_authority.h"
#include "kmsp11/object_store_state.pb.h"
#include "kmsp11/util/crypto_utils.h"
#include "kmsp11/util/errors.h"
#include "kmsp11/util/kms_client.h"
#include "kmsp11/util/status_macros.h"
#include "kmsp11/util/string_utils.h"

namespace kmsp11 {
namespace {

static absl::StatusOr<CK_SLOT_INFO> NewSlotInfo() {
  CK_SLOT_INFO info = {
      {0},                // slotDescription (set with ' ' padding below)
      {0},                // manufacturerID (set with ' ' padding below)
      CKF_TOKEN_PRESENT,  // flags
      {0, 0},             // hardwareVersion
      {0, 0},             // firmwareVersion
  };
  RETURN_IF_ERROR(
      CryptokiStrCopy("A virtual slot mapped to a key ring in Google Cloud KMS",
                      info.slotDescription));
  RETURN_IF_ERROR(CryptokiStrCopy("Google", info.manufacturerID));
  return info;
}

static absl::StatusOr<CK_TOKEN_INFO> NewTokenInfo(
    absl::string_view token_label) {
  CK_TOKEN_INFO info = {
      {0},  // label (set with ' ' padding below)
      {0},  // manufacturerID (set with ' ' padding below)
      {0},  // model (set with ' ' padding below)
      {0},  // serialNumber (set below)
      CKF_USER_PIN_INITIALIZED | CKF_TOKEN_INITIALIZED |
          CKF_SO_PIN_LOCKED,       // flags
      CK_EFFECTIVELY_INFINITE,     // ulMaxSessionCount
      CK_UNAVAILABLE_INFORMATION,  // ulSessionCount
      CK_EFFECTIVELY_INFINITE,     // ulMaxRwSessionCount
      CK_UNAVAILABLE_INFORMATION,  // ulRwSessionCount
      0,                           // ulMaxPinLen
      0,                           // ulMinPinLen
      CK_UNAVAILABLE_INFORMATION,  // ulTotalPublicMemory
      CK_UNAVAILABLE_INFORMATION,  // ulFreePublicMemory
      CK_UNAVAILABLE_INFORMATION,  // ulTotalPrivateMemory
      CK_UNAVAILABLE_INFORMATION,  // ulFreePrivateMemory
      {0, 0},                      // hardwareVersion
      {0, 0},                      // firmwareVersion
      {0}                          // utcTime (set below)
  };
  RETURN_IF_ERROR(CryptokiStrCopy(token_label, info.label));
  RETURN_IF_ERROR(CryptokiStrCopy("Google", info.manufacturerID));
  RETURN_IF_ERROR(CryptokiStrCopy("Cloud KMS Token", info.model));
  RETURN_IF_ERROR(CryptokiStrCopy("", info.serialNumber, '0'));
  RETURN_IF_ERROR(CryptokiStrCopy("", info.utcTime, '0'));
  return info;
}

class ObjectStoreBuilder {
 public:
  static absl::StatusOr<ObjectStoreBuilder> New(bool generate_certs) {
    ObjectStoreBuilder builder;
    if (generate_certs) {
      ASSIGN_OR_RETURN(builder.cert_authority_, CertAuthority::New());
    }
    return std::move(builder);
  }

  const ObjectStoreState& state() { return state_; }

  absl::Status AddAsymmetricKey(const kms_v1::CryptoKeyVersion& ckv,
                                const kms_v1::PublicKey& public_key) {
    ASSIGN_OR_RETURN(bssl::UniquePtr<EVP_PKEY> pub,
                     ParseX509PublicKeyPem(public_key.pem()));

    AsymmetricKey key;
    *key.mutable_crypto_key_version() = ckv;
    key.set_private_key_handle(NewHandle());
    key.set_public_key_handle(NewHandle());
    ASSIGN_OR_RETURN(*key.mutable_public_key_der(),
                     MarshalX509PublicKeyDer(pub.get()));

    if (cert_authority_) {
      ASSIGN_OR_RETURN(bssl::UniquePtr<X509> x509,
                       cert_authority_->GenerateCert(ckv, pub.get()));
      ASSIGN_OR_RETURN(*key.mutable_certificate()->mutable_x509_der(),
                       MarshalX509CertificateDer(x509.get()))
      key.mutable_certificate()->set_handle(NewHandle());
    }

    *state_.add_asymmetric_keys() = key;
    return absl::OkStatus();
  }

 private:
  CK_OBJECT_HANDLE NewHandle() {
    CK_OBJECT_HANDLE handle;
    do {
      handle = RandomHandle();
    } while (allocated_handles_.contains(handle));
    allocated_handles_.insert(handle);
    return handle;
  }

  ObjectStoreBuilder() {}

  ObjectStoreState state_;
  std::unique_ptr<CertAuthority> cert_authority_;
  absl::flat_hash_set<CK_OBJECT_HANDLE> allocated_handles_;
};

absl::Status LoadVersions(const KmsClient& client, const kms_v1::CryptoKey& key,
                          ObjectStoreBuilder* builder) {
  kms_v1::ListCryptoKeyVersionsRequest req;
  req.set_parent(key.name());
  CryptoKeyVersionsRange v = client.ListCryptoKeyVersions(req);

  for (CryptoKeyVersionsRange::iterator it = v.begin(); it != v.end(); it++) {
    ASSIGN_OR_RETURN(kms_v1::CryptoKeyVersion ckv, *it);
    if (ckv.state() != kms_v1::CryptoKeyVersion_CryptoKeyVersionState_ENABLED) {
      LOG(INFO) << "skipping version " << ckv.name() << " with state "
                << ckv.state();
      continue;
    }
    if (!GetDetails(ckv.algorithm()).ok()) {
      LOG(INFO) << "skipping version " << ckv.name()
                << " with unsuported algorithm " << ckv.algorithm();
      continue;
    }

    kms_v1::GetPublicKeyRequest pub_req;
    pub_req.set_name(ckv.name());

    ASSIGN_OR_RETURN(kms_v1::PublicKey pub, client.GetPublicKey(pub_req));
    RETURN_IF_ERROR(builder->AddAsymmetricKey(ckv, pub));
  }

  return absl::OkStatus();
}

absl::StatusOr<ObjectStoreState> LoadState(const KmsClient& client,
                                           absl::string_view key_ring_name,
                                           bool generate_certs) {
  std::unique_ptr<CertAuthority> cert_authority;

  ASSIGN_OR_RETURN(ObjectStoreBuilder builder,
                   ObjectStoreBuilder::New(generate_certs));

  kms_v1::ListCryptoKeysRequest req;
  req.set_parent(std::string(key_ring_name));
  CryptoKeysRange keys = client.ListCryptoKeys(req);

  for (CryptoKeysRange::iterator it = keys.begin(); it != keys.end(); it++) {
    ASSIGN_OR_RETURN(kms_v1::CryptoKey key, *it);

    if (key.version_template().protection_level() !=
        kms_v1::ProtectionLevel::HSM) {
      LOG(INFO) << "skipping key " << key.name()
                << " with unsupported protection level "
                << key.version_template().protection_level();
      continue;
    }

    switch (key.purpose()) {
      case kms_v1::CryptoKey::ASYMMETRIC_DECRYPT:
      case kms_v1::CryptoKey::ASYMMETRIC_SIGN:
        RETURN_IF_ERROR(LoadVersions(client, key, &builder));
        break;
      default:
        LOG(INFO) << "skipping key " << key.name()
                  << " with unsupported purpose " << key.purpose();
        continue;
    }
  }
  return builder.state();
}

absl::StatusOr<std::unique_ptr<HandleMap<Object>>> LoadObjects(
    const ObjectStoreState& state) {
  auto objects =
      absl::make_unique<HandleMap<Object>>(CKR_OBJECT_HANDLE_INVALID);
  for (const AsymmetricKey& key : state.asymmetric_keys()) {
    ASSIGN_OR_RETURN(bssl::UniquePtr<EVP_PKEY> pub,
                     ParseX509PublicKeyDer(key.public_key_der()));
    ASSIGN_OR_RETURN(KeyPair key_pair,
                     Object::NewKeyPair(key.crypto_key_version(), pub.get()));

    RETURN_IF_ERROR(objects->AddDirect(
        key.public_key_handle(),
        std::make_shared<Object>(std::move(key_pair.public_key))));
    RETURN_IF_ERROR(objects->AddDirect(
        key.private_key_handle(),
        std::make_shared<Object>(std::move(key_pair.private_key))));

    if (key.has_certificate()) {
      ASSIGN_OR_RETURN(bssl::UniquePtr<X509> x509,
                       ParseX509CertificateDer(key.certificate().x509_der()));
      ASSIGN_OR_RETURN(Object cert, Object::NewCertificate(
                                        key.crypto_key_version(), x509.get()));
      RETURN_IF_ERROR(
          objects->AddDirect(key.certificate().handle(),
                             std::make_shared<Object>(std::move(cert))));
    }
  }
  return std::move(objects);
}

}  // namespace

absl::StatusOr<std::unique_ptr<Token>> Token::New(CK_SLOT_ID slot_id,
                                                  TokenConfig token_config,
                                                  KmsClient* kms_client,
                                                  bool generate_certs) {
  ASSIGN_OR_RETURN(CK_SLOT_INFO slot_info, NewSlotInfo());
  ASSIGN_OR_RETURN(CK_TOKEN_INFO token_info,
                   NewTokenInfo(token_config.label()));

  ASSIGN_OR_RETURN(
      ObjectStoreState state,
      LoadState(*kms_client, token_config.key_ring(), generate_certs));
  ASSIGN_OR_RETURN(std::unique_ptr<HandleMap<Object>> objects,
                   LoadObjects(state));

  // using `new` to invoke a private constructor
  return std::unique_ptr<Token>(
      new Token(slot_id, slot_info, token_info, std::move(objects)));
}

bool Token::is_logged_in() const {
  absl::ReaderMutexLock l(&login_mutex_);
  return is_logged_in_;
}

absl::Status Token::Login(CK_USER_TYPE user) {
  switch (user) {
    case CKU_USER:
      break;
    case CKU_SO:
      return NewError(absl::StatusCode::kPermissionDenied,
                      "login as CKU_SO is not permitted", CKR_PIN_LOCKED,
                      SOURCE_LOCATION);
    case CKU_CONTEXT_SPECIFIC:
      // See description of CKA_ALWAYS_AUTHENTICATE at
      // http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc322855286
      return NewError(absl::StatusCode::kPermissionDenied,
                      "CKA_ALWAYS_AUTHENTICATE is not true on this token",
                      CKR_OPERATION_NOT_INITIALIZED, SOURCE_LOCATION);
    default:
      return NewInvalidArgumentError(
          absl::StrFormat("unknown user type: %#x", user),
          CKR_USER_TYPE_INVALID, SOURCE_LOCATION);
  }

  absl::WriterMutexLock l(&login_mutex_);
  if (is_logged_in_) {
    return FailedPreconditionError("user is already logged in",
                                   CKR_USER_ALREADY_LOGGED_IN, SOURCE_LOCATION);
  }
  is_logged_in_ = true;
  return absl::OkStatus();
}

absl::Status Token::Logout() {
  absl::WriterMutexLock l(&login_mutex_);
  if (!is_logged_in_) {
    return FailedPreconditionError("user is not logged in",
                                   CKR_USER_NOT_LOGGED_IN, SOURCE_LOCATION);
  }
  is_logged_in_ = false;
  return absl::OkStatus();
}

std::vector<CK_OBJECT_HANDLE> Token::FindObjects(
    std::function<bool(const Object&)> predicate) const {
  return objects_->Find(predicate,
                        [](const Object& o1, const Object& o2) -> bool {
                          if (o1.kms_key_name() == o2.kms_key_name()) {
                            return o1.object_class() < o2.object_class();
                          }
                          return o1.kms_key_name() < o2.kms_key_name();
                        });
}

}  // namespace kmsp11
